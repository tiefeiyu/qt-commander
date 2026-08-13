#include "selector.h"
#include "../compat_qt.h"
#include "../core/ui_scanner.h"

#include <QApplication>
#include <QGuiApplication>
#include <QJsonValue>
#include <QMetaObject>
#include <QMetaProperty>
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QQmlContext>
#endif
#include <QQueue>
#include <QSet>
#include <QWidget>
#include <QWindow>
#include <QtMath>

#include <cmath>
#include <limits>

// ======================================================================
// Internal helpers
// ======================================================================

namespace {

// -----------------------------------------------------------------------
// Safe numeric comparison between two QVariants
//
// QJsonValue::toVariant() may produce int, double, or qint64 for numeric
// JSON values depending on the Qt version.  QObject::property() returns
// the native type (int, uint, qlonglong, double, etc.).  We attempt
// integer comparison first, then floating-point, then fall through to
// QVariant::operator== for exact-type matches.
// -----------------------------------------------------------------------
bool variantsEqual(const QVariant& lhs, const QVariant& rhs)
{
    // --- both null / invalid ------------------------------------------------
    if (lhs.isNull() && rhs.isNull()) return true;
    if (lhs.isNull() != rhs.isNull()) return false;
    if (!lhs.isValid() && !rhs.isValid()) return true;
    if (!lhs.isValid() || !rhs.isValid()) return false;

    // --- integral comparison -------------------------------------------------
    {
        bool lhsOk = false, rhsOk = false;
        const qlonglong lhsInt = lhs.toLongLong(&lhsOk);
        const qlonglong rhsInt = rhs.toLongLong(&rhsOk);
        if (lhsOk && rhsOk) {
            return lhsInt == rhsInt;
        }
    }

    // --- unsigned integral comparison (avoid sign-extension issues) ----------
    {
        bool lhsOk = false, rhsOk = false;
        const qulonglong lhsUint = lhs.toULongLong(&lhsOk);
        const qulonglong rhsUint = rhs.toULongLong(&rhsOk);
        if (lhsOk && rhsOk) {
            return lhsUint == rhsUint;
        }
    }

    // --- floating-point comparison (with tolerance) --------------------------
    {
        bool lhsOk = false, rhsOk = false;
        const double lhsDbl = lhs.toDouble(&lhsOk);
        const double rhsDbl = rhs.toDouble(&rhsOk);
        if (lhsOk && rhsOk) {
            if (std::isnan(lhsDbl) && std::isnan(rhsDbl)) return true;
            // Use a simple absolute-difference comparison; integers that
            // didn't parse as integral will land here as doubles.
            return qFuzzyCompare(lhsDbl, rhsDbl);
        }
    }

    // --- boolean -------------------------------------------------------------
    if (lhs.userType() == QMetaType::Bool &&
        rhs.userType() == QMetaType::Bool) {
        return lhs.toBool() == rhs.toBool();
    }

    // --- string fallback -----------------------------------------------------
    if (lhs.canConvert<QString>() && rhs.canConvert<QString>()) {
        return lhs.toString() == rhs.toString();
    }

    // --- QVariant::operator== (last resort) ----------------------------------
    return lhs == rhs;
}

// -----------------------------------------------------------------------
// Enumerate the child QObjects that should be traversed when walking the
// UI tree from `obj`.
//
// For QQuickItems we use childItems() (visual children in scene-graph
// order).  For all other QObjects we use QObject::children().
// -----------------------------------------------------------------------
QVector<QObject*> childrenForTraversal(QObject* obj)
{
    QVector<QObject*> result;

#ifdef QT_COMMANDER_WITH_QML
    if (QQuickItem* item = qobject_cast<QQuickItem*>(obj)) {
        const auto items = item->childItems();
        result.reserve(items.size());
        for (QQuickItem* ci : items) {
            result.append(static_cast<QObject*>(ci));
        }
        return result;
    }
#endif

    // QWidget, QWindow, QObject, etc. -- use QObject children.
    const QObjectList& list = obj->children();
    result.reserve(list.size());
    for (QObject* child : list) {
        result.append(child);
    }
    return result;
}

// -----------------------------------------------------------------------
// Collect the root QObject pointers from which the walk should start.
// -----------------------------------------------------------------------
QVector<QObject*> collectRoots(
    const QHash<uint64_t, QObject*>& element_map,
    uint64_t ancestor_id,
    uint64_t window_id)
{
    QVector<QObject*> roots;

    // 1. Explicit ancestor -- walk only its subtree.
    if (ancestor_id > 0) {
        QObject* ancestor = element_map.value(ancestor_id, nullptr);
        if (ancestor) {
            roots.append(ancestor);
            return roots;
        }
        // Ancestor not found: return empty (caller will get no results).
        return roots;
    }

    // 2. Explicit window.
    if (window_id > 0) {
        QObject* winObj = element_map.value(window_id, nullptr);
        if (!winObj) {
            return roots;   // window not found -- empty results
        }

#ifdef QT_COMMANDER_WITH_QML
        if (QQuickWindow* qw = qobject_cast<QQuickWindow*>(winObj)) {
            if (qw->contentItem()) {
                roots.append(qw->contentItem());
            }
        } else
#endif
        if (QWidget* w = qobject_cast<QWidget*>(winObj)) {
            roots.append(w);
        } else if (QWindow* win = qobject_cast<QWindow*>(winObj)) {
            // Plain QWindow -- try to find its widget counterpart.
            QWidget* w = QWidget::find(win->winId());
            roots.append(w ? static_cast<QObject*>(w) : winObj);
        } else {
            roots.append(winObj);
        }
        return roots;
    }

    // 3. No ancestor / no window -- walk all top-level windows.
    const QList<QWindow*> toplevels = QGuiApplication::topLevelWindows();
    for (QWindow* win : toplevels) {
        // Widget-backed window
        if (QWidget* w = QWidget::find(win->winId())) {
            roots.append(w);
        }
#ifdef QT_COMMANDER_WITH_QML
        // QQuickWindow
        if (QQuickWindow* qw = qobject_cast<QQuickWindow*>(win)) {
            if (qw->contentItem()) {
                roots.append(qw->contentItem());
            }
        }
#endif
        // Plain QWindow with no widget backing
#ifdef QT_COMMANDER_WITH_QML
        if (!QWidget::find(win->winId()) && !qobject_cast<QQuickWindow*>(win)) {
#else
        if (!QWidget::find(win->winId())) {
#endif
            roots.append(win);
        }
    }

    return roots;
}

// -----------------------------------------------------------------------
// BFS walk implementation shared by find() and internal helpers.
// Returns true if the result limit was reached (caller should stop).
// -----------------------------------------------------------------------
struct WalkState {
    QVector<SelectorResult>& results;
    const QJsonObject&       query;
    const QHash<uint64_t, QObject*>& element_map;
    uint64_t                 ancestor_id;
    uint64_t                 window_id;
    int                      max_depth;
    int                      limit;
    int                      count;       // number of results collected so far
};

/// Build a reverse map (QObject* -> id) from the element_map for O(1)
/// id lookups during matching.
static QHash<QObject*, uint64_t> invertMap(
    const QHash<uint64_t, QObject*>& element_map)
{
    QHash<QObject*, uint64_t> rev;
    rev.reserve(element_map.size());
    for (auto it = element_map.constBegin(); it != element_map.constEnd(); ++it) {
        rev.insert(it.value(), it.key());
    }
    return rev;
}

static bool walkBfs(WalkState& state, const QVector<QObject*>& roots,
                    const QHash<QObject*, uint64_t>& rev_map)
{
    // Queue: (object, depth)
    struct Entry { QObject* obj; int depth; };
    QQueue<Entry> queue;
    for (QObject* r : roots) {
        queue.enqueue({r, 0});
    }

    // Guard against duplicate traversal: some widgets are reachable from
    // more than one root (e.g. a combo popup's QRollEffect is both a child
    // of the popup container and a top-level widget, so its descendants --
    // including the list view -- would otherwise be visited twice).
    QSet<QObject*> visited;

    while (!queue.isEmpty()) {
        Entry e = queue.dequeue();
        if (visited.contains(e.obj))
            continue;
        visited.insert(e.obj);

        // ---- match check ---------------------------------------------------
        if (ElementSelector::matchesQuery(e.obj, state.query,
                                           state.element_map,
                                           state.ancestor_id,
                                           state.window_id)) {
            SelectorResult sr;
            sr.id     = rev_map.value(e.obj, 0);
            sr.object = e.obj;
            state.results.append(sr);
            state.count++;

            if (state.limit > 0 && state.count >= state.limit) {
                return true;   // limit reached -- stop
            }
        }

        // ---- enqueue children (if depth allows) ----------------------------
        if (state.max_depth < 0 || e.depth < state.max_depth) {
            const QVector<QObject*> kids = childrenForTraversal(e.obj);
            for (QObject* kid : kids) {
                queue.enqueue({kid, e.depth + 1});
            }
        }
    }

    return false;   // finished without hitting the limit
}

} // anonymous namespace

// ======================================================================
// ElementSelector public API
// ======================================================================

QVector<SelectorResult> ElementSelector::find(
    const QJsonObject& query,
    const QHash<uint64_t, QObject*>& element_map)
{
    // --------------- parse query parameters -------------------------------
    const uint64_t ancestor_id = static_cast<uint64_t>(
        query.value(QStringLiteral("ancestor_id")).toDouble(0.0));
    const uint64_t window_id = static_cast<uint64_t>(
        query.value(QStringLiteral("window_id")).toDouble(0.0));
    const QString depthStr = query.value(QStringLiteral("depth"))
                                  .toString(QStringLiteral("deep"));
    const int limit = query.value(QStringLiteral("limit")).toInt(0);

    // max_depth: -1 = unlimited, 0+ = exact cap on recursion levels.
    // "exact"   -> depth 1 (direct children of the root)
    // "shallow" -> depth 2
    // "deep"    -> unlimited (-1)
    int max_depth = -1;
    if (depthStr == QLatin1String("exact")) {
        max_depth = 1;
    } else if (depthStr == QLatin1String("shallow")) {
        max_depth = 2;
    }

    // --------------- resolve roots ----------------------------------------
    const QVector<QObject*> roots = collectRoots(element_map,
                                                  ancestor_id,
                                                  window_id);
    if (roots.isEmpty()) {
        return {};   // nothing to walk
    }

    // --------------- build reverse map for O(1) id lookups -----------------
    const QHash<QObject*, uint64_t> rev_map = invertMap(element_map);

    // --------------- BFS walk ----------------------------------------------
    QVector<SelectorResult> results;
    WalkState state{
        results,
        query,
        element_map,
        ancestor_id,
        window_id,
        max_depth,
        (limit > 0) ? limit : std::numeric_limits<int>::max(),
        0
    };

    walkBfs(state, roots, rev_map);

    return results;
}

// ======================================================================
// ElementSelector private helpers
// ======================================================================

bool ElementSelector::matchesQuery(
    QObject* obj,
    const QJsonObject& query,
    const QHash<uint64_t, QObject*>& element_map,
    const uint64_t query_ancestor_id,
    const uint64_t query_window_id)
{
    if (!obj) return false;

    // ---- visibility: hidden elements are excluded unless the query asks
    // for them explicitly.  Matches the snapshot's include_hidden contract:
    // callers interact with what they can see, so find results should not
    // contain invisible elements (whose ids are unusable for clicks, etc.).
    if (!query.value(QStringLiteral("include_hidden")).toBool(false)) {
        if (!UiScanner::isEffectivelyVisible(obj)) {
            return false;
        }
    }

    // ---- type: exact class name match ------------------------------------
    const QJsonValue typeVal = query.value(QStringLiteral("type"));
    if (!typeVal.isUndefined()) {
        const QString expectedType = typeVal.toString();
        if (expectedType != QLatin1String(obj->metaObject()->className())) {
            return false;
        }
    }

    // ---- type_inherits: walk superClass chain ----------------------------
    const QJsonValue inheritsVal = query.value(QStringLiteral("type_inherits"));
    if (!inheritsVal.isUndefined()) {
        const QString expectedInherits = inheritsVal.toString();
        bool found = false;
        const QMetaObject* mo = obj->metaObject();
        while (mo) {
            if (expectedInherits == QLatin1String(mo->className())) {
                found = true;
                break;
            }
            mo = mo->superClass();
        }
        if (!found) return false;
    }

    // ---- text: exact display text match ----------------------------------
    const QJsonValue textVal = query.value(QStringLiteral("text"));
    if (!textVal.isUndefined()) {
        const QString expectedText = textVal.toString();
        if (getDisplayText(obj) != expectedText) {
            return false;
        }
    }

    // ---- text_contains: substring match ----------------------------------
    const QJsonValue textContainsVal = query.value(
        QStringLiteral("text_contains"));
    if (!textContainsVal.isUndefined()) {
        const QString needle = textContainsVal.toString();
        if (!getDisplayText(obj).contains(needle)) {
            return false;
        }
    }

    // ---- object_name: exact QObject::objectName match --------------------
    const QJsonValue objNameVal = query.value(QStringLiteral("object_name"));
    if (!objNameVal.isUndefined()) {
        const QString expectedName = objNameVal.toString();
        if (obj->objectName() != expectedName) {
            return false;
        }
    }

    // ---- qml_id: QML id match (QQmlContext::nameForObject) ----------------
    // The QML `id` is what QML developers reference in source; it is
    // distinct from objectName.  An empty expected value is ignored
    // (every unnamed object would otherwise match).
#ifdef QT_COMMANDER_WITH_QML
    const QJsonValue qmlIdVal = query.value(QStringLiteral("qml_id"));
    if (!qmlIdVal.isUndefined()) {
        const QString expectedId = qmlIdVal.toString();
        if (!expectedId.isEmpty() && UiScanner::qmlId(obj) != expectedId) {
            return false;
        }
    }
#endif

    // ---- window_title: containing window title exact match ---------------
    const QJsonValue winTitleVal = query.value(QStringLiteral("window_title"));
    if (!winTitleVal.isUndefined()) {
        const QString expectedTitle = winTitleVal.toString();
        QObject* win = getContainingWindow(obj);
        if (!win) return false;
        QString actualTitle;
        if (QWidget* w = qobject_cast<QWidget*>(win)) {
            actualTitle = w->windowTitle();
        } else if (QWindow* qw = qobject_cast<QWindow*>(win)) {
            actualTitle = qw->title();
        }
        if (actualTitle != expectedTitle) return false;
    }

    // ---- window_title_contains: substring on window title -----------------
    const QJsonValue winTitleCVal = query.value(
        QStringLiteral("window_title_contains"));
    if (!winTitleCVal.isUndefined()) {
        const QString needle = winTitleCVal.toString();
        QObject* win = getContainingWindow(obj);
        if (!win) return false;
        QString actualTitle;
        if (QWidget* w = qobject_cast<QWidget*>(win)) {
            actualTitle = w->windowTitle();
        } else if (QWindow* qw = qobject_cast<QWindow*>(win)) {
            actualTitle = qw->title();
        }
        if (!actualTitle.contains(needle)) return false;
    }

    // ---- properties: ALL key-value pairs must match ----------------------
    const QJsonValue propsVal = query.value(QStringLiteral("properties"));
    if (!propsVal.isUndefined()) {
        const QJsonObject props = propsVal.toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QString propName  = it.key();
            const QVariant expected = it.value().toVariant();
            const QVariant actual   = getPropertyValue(obj, propName);
            if (!actual.isValid()) {
                // Property does not exist on this object.
                return false;
            }
            if (!variantsEqual(actual, expected)) {
                return false;
            }
        }
    }

    // ---- ancestor_id: ensure this object is a descendant of the ancestor --
    if (query_ancestor_id > 0) {
        QObject* ancestor = element_map.value(query_ancestor_id, nullptr);
        if (!ancestor) return false;
        // Walk parent chain to verify descendant relationship.
        bool isDescendant = false;
        QObject* p = obj->parent();
        while (p) {
            if (p == ancestor) {
                isDescendant = true;
                break;
            }
            p = p->parent();
        }
        if (!isDescendant) return false;
    }

    // ---- window_id: ensure object lives in the specified window ----------
    if (query_window_id > 0) {
        QObject* winObj = element_map.value(query_window_id, nullptr);
        if (!winObj) return false;
        QObject* actualWindow = getContainingWindow(obj);
        if (actualWindow != winObj) return false;
    }

    // All checks passed.
    return true;
}

// -----------------------------------------------------------------------
// getPropertyValue
// -----------------------------------------------------------------------
QVariant ElementSelector::getPropertyValue(QObject* obj, const QString& name)
{
    if (!obj) return {};

    const QByteArray nameBa = name.toUtf8();

    // 1. Q_PROPERTY via meta-object system
    const QMetaObject* mo = obj->metaObject();
    if (mo) {
        const int idx = mo->indexOfProperty(nameBa.constData());
        if (idx >= 0) {
            QMetaProperty prop = mo->property(idx);
            if (prop.isReadable()) {
                return prop.read(obj);
            }
        }
    }

    // 2. Dynamic property (QObject::setProperty / QQmlProperty)
    //    QObject::property() also catches Q_PROPERTY entries, so we only
    //    call it as a fallback for dynamic properties.
    const QVariant dynamic = obj->property(nameBa.constData());
    if (dynamic.isValid()) {
        return dynamic;
    }

    return {};
}

// -----------------------------------------------------------------------
// getDisplayText / getContainingWindow  --  delegated to UiScanner so the
// snapshot and findElement paths share one text/window contract.
// -----------------------------------------------------------------------
QString ElementSelector::getDisplayText(QObject* obj)
{
    return UiScanner::displayText(obj);
}

QObject* ElementSelector::getContainingWindow(QObject* obj)
{
    return UiScanner::getContainingWindow(obj);
}
