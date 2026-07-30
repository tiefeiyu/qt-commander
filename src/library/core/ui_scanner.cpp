#include "ui_scanner.h"
#include "element_map.h"
#include "../compat_qt.h"

#include <QApplication>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>
#include <QStack>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaEnum>
#include <QFile>
#include <QDir>
#ifdef QT_COMMANDER_WITH_QML
#include <QtQuickWidgets/QQuickWidget>
#include <QQuickWindow>
#include <QQuickItem>
#endif
#include <QDateTime>
#include <QPixmap>
#include <QImage>
#include <QScreen>
#include <QPoint>
#include <QSize>
#include <QRect>
#include <QColor>
#include <QFont>
#include <QUrl>
#include <QBuffer>
#include <cstring>

// ============================================================================
// generateSnapshot  —  entry point
// ============================================================================
QJsonObject UiScanner::generateSnapshot(const QString& session_id,
                                         int snapshot_id,
                                         const QString& detail,
                                         bool include_hidden,
                                         ElementMap* element_map,
                                         const QString& snapshot_dir)
{
    QJsonObject result;

    if (!element_map) {
        result["error"] = QStringLiteral("ElementMap is null");
        return result;
    }

    // Clear previous state and advance epoch (each method handles its own locking).
    element_map->clear();
    element_map->incrementEpoch();

    result["session_id"]    = session_id;
    result["snapshot_id"]   = snapshot_id;
    result["epoch"]         = static_cast<qint64>(element_map->epoch());
    result["timestamp_ms"]  = QDateTime::currentMSecsSinceEpoch();
    result["detail"]        = detail;

    // Ensure snapshot directory exists for "full" detail.
    if (detail == QStringLiteral("full") && !snapshot_dir.isEmpty()) {
        QDir().mkpath(snapshot_dir);
        QDir().mkpath(snapshot_dir + QStringLiteral("/props"));
    }

    QJsonArray elements;
    QHash<QObject*, uint64_t> id_map;
    uint64_t next_id = 1;
    QSet<QObject*> visited;
    const int maxDepth = 1000;
    bool truncated = false;
    QString truncReason;

    // Pass 1: traverse the live object trees, assign IDs, build JSON skeletons.
    traverseWidgets(elements, element_map, id_map, next_id, visited,
                    maxDepth, detail, include_hidden, snapshot_dir,
                    truncated, truncReason);

#ifdef QT_COMMANDER_WITH_QML
    if (!truncated) {
        traverseQmlWindows(elements, element_map, id_map, next_id, visited,
                           maxDepth, detail, include_hidden, snapshot_dir,
                           truncated, truncReason);
    }
#endif

    // Pass 2: compute child_indices from parent_id relationships.
    // Build a parent-id -> list-of-child-indices map.
    QHash<uint64_t, QJsonArray> parentToChildren;
    for (int i = 0; i < elements.size(); ++i) {
        const QJsonObject el = elements[i].toObject();
        uint64_t parentId = static_cast<uint64_t>(el.value(QStringLiteral("parent_id")).toDouble(0));
        if (parentId > 0) {
            QJsonArray siblings = parentToChildren.value(parentId);
            siblings.append(i);
            parentToChildren[parentId] = siblings;
        }
    }

    // Inject child_indices into each element JSON.
    for (int i = 0; i < elements.size(); ++i) {
        QJsonObject el = elements[i].toObject();
        uint64_t elId = static_cast<uint64_t>(el.value(QStringLiteral("id")).toDouble());
        QJsonArray children = parentToChildren.value(elId);
        el[QStringLiteral("child_indices")] = children;
        elements[i] = el;
    }

#ifdef QT_COMMANDER_WITH_QML
    // Diagnostic: what top-level windows exist?
    {
        QJsonArray tw;
        for (auto* w : QGuiApplication::topLevelWindows()) {
            QJsonObject o;
            o["class"] = QString::fromLatin1(w->metaObject()->className());
            o["isQuick"] = (qobject_cast<QQuickWindow*>(w) != nullptr);
            o["visible"] = w->isVisible();
            tw.append(o);
        }
        result["_debug_topLevelWindows"] = tw;
    }
#endif

    result["element_count"] = elements.size();
    result["elements"]      = elements;

    if (truncated) {
        result["truncated"]        = true;
        result["truncated_reason"] = truncReason;
    }

    return result;
}

// ============================================================================
// traverseWidgets  —  iterative QStack traversal over QWidget tree
// ============================================================================
void UiScanner::traverseWidgets(QJsonArray& elements,
                                 ElementMap* element_map,
                                 QHash<QObject*, uint64_t>& id_map,
                                 uint64_t& next_id,
                                 QSet<QObject*>& visited,
                                 int maxDepth,
                                 const QString& detail,
                                 bool include_hidden,
                                 const QString& snapshot_dir,
                                 bool& truncated,
                                 QString& truncReason)
{
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app)
        return;

    const QWidgetList allWidgets = app->allWidgets();

    struct WStackFrame {
        QWidget* widget;
        QWidget* visualParent;   // nullptr for top-level windows
    };

    for (QWidget* topWidget : allWidgets) {
        if (!topWidget->isWindow())
            continue;
        if (!include_hidden && !isEffectivelyVisible(topWidget))
            continue;

        QStack<WStackFrame> stack;
        stack.push({topWidget, nullptr});

        while (!stack.isEmpty()) {
            if (visited.size() >= maxDepth) {
                truncated = true;
                truncReason = QStringLiteral("Max element count reached");
                return;
            }

            WStackFrame frame = stack.pop();
            QWidget* w = frame.widget;
            if (!w || visited.contains(w))
                continue;

            // Cycle detection
            visited.insert(w);

            // Assign ID
            uint64_t id = next_id++;
            element_map->insert(id, w);
            id_map.insert(w, id);

            // Serialize
            QJsonObject el = serializeElement(w, frame.visualParent, id,
                                               detail, snapshot_dir, id_map);
            elements.append(el);

            // Push children in reverse order so they're processed
            // left-to-right / top-to-bottom.
            const QObjectList& children = w->children();
            QList<QWidget*> widgetChildren;
            for (QObject* child : children) {
                if (auto* childW = qobject_cast<QWidget*>(child)) {
                    if (!include_hidden && !isEffectivelyVisible(childW))
                        continue;
                    widgetChildren.append(childW);
                }
            }
            // Reverse order for QStack (LIFO) so first child is processed first.
            for (int ci = widgetChildren.size() - 1; ci >= 0; --ci) {
                stack.push({widgetChildren[ci], w});
            }
        }
    }
}

#ifdef QT_COMMANDER_WITH_QML
// ============================================================================
// traverseQmlWindows  —  walk QQuickItem tree via recursion (trees are shallow)
// ============================================================================
void UiScanner::traverseQmlWindows(QJsonArray& elements,
                                    ElementMap* element_map,
                                    QHash<QObject*, uint64_t>& id_map,
                                    uint64_t& next_id,
                                    QSet<QObject*>& visited,
                                    int maxDepth,
                                    const QString& detail,
                                    bool include_hidden,
                                    const QString& snapshot_dir,
                                    bool& truncated,
                                    QString& truncReason)
{
    QGuiApplication* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!guiApp)
        return;

    const auto topLevelWindows = QGuiApplication::topLevelWindows();

    // Recursive lambda for walking the QQuickItem tree.
    // Using a struct with a Y-combinator-style self-reference.
    struct QmlWalker {
        QJsonArray* elems;
        ElementMap* map;
        QHash<QObject*, uint64_t>* idm;
        uint64_t* nid;
        QSet<QObject*>* vis;
        int maxD;
        const QString* det;
        bool inclHidden;
        const QString* snapDir;
        bool* truncd;
        QString* trunRsn;

        void walk(QQuickItem* item, QQuickItem* visualParent) {
            if (!item || vis->contains(item))
                return;
            if (vis->size() >= maxD) {
                *truncd = true;
                *trunRsn = QStringLiteral("Max element count reached");
                return;
            }

            vis->insert(item);

            uint64_t id = (*nid)++;
            map->insert(id, item);
            idm->insert(item, id);

            QJsonObject el = UiScanner::serializeElement(item, visualParent, id,
                                                          *det, *snapDir, *idm);
            elems->append(el);

            // Recurse into children (back-to-front for stable ordering).
            const auto childItems = item->childItems();
            for (int ci = childItems.size() - 1; ci >= 0; --ci) {
                QQuickItem* child = childItems.at(ci);
                if (!inclHidden && !UiScanner::isEffectivelyVisible(child))
                    continue;
                walk(child, item);
                if (*truncd)
                    return;
            }
        }
    };

    QmlWalker walker{&elements, element_map, &id_map, &next_id, &visited,
                     maxDepth, &detail, include_hidden, &snapshot_dir,
                     &truncated, &truncReason};

    // DEBUG: add diagnostic to snapshot result
    QJsonArray debugWindows;
    for (QWindow* win : topLevelWindows) {
        QJsonObject dw;
        dw["className"] = QString::fromLatin1(win->metaObject()->className());
        dw["isQuickWindow"] = (qobject_cast<QQuickWindow*>(win) != nullptr);
        dw["visible"] = win->isVisible();
        debugWindows.append(dw);

        auto* quickWin = qobject_cast<QQuickWindow*>(win);
        if (!quickWin)
            continue;

        QQuickItem* contentItem = quickWin->contentItem();
        if (!contentItem)
            continue;

        // Serialize the window itself first.
        if (!visited.contains(quickWin)) {
            visited.insert(quickWin);
            uint64_t id = next_id++;
            element_map->insert(id, quickWin);
            id_map.insert(quickWin, id);

            QJsonObject el = serializeElement(quickWin, nullptr, id,
                                               detail, snapshot_dir, id_map);
            elements.append(el);
        }

        // Walk the content item subtree.
        if (!include_hidden && !isEffectivelyVisible(contentItem))
            continue;

        walker.walk(contentItem, nullptr);

        if (truncated)
            return;
    }

    // Also scan QQuickWidget children in the QWidget tree.
    // QQuickWidget creates offscreen QQuickWindows that don't appear in topLevelWindows().
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        const auto widgets = app->allWidgets();
        for (QWidget* w : widgets) {
            auto* qqw = qobject_cast<QQuickWidget*>(w);
            if (!qqw) continue;
            QQuickWindow* qw = qqw->quickWindow();
            if (!qw) continue;
            QQuickItem* ci = qw->contentItem();
            if (!ci) continue;
            // Serialize the QQuickWindow if not already visited
            if (!visited.contains(qw)) {
                visited.insert(qw);
                uint64_t id = next_id++;
                element_map->insert(id, qw);
                id_map.insert(qw, id);
                QJsonObject el = serializeElement(qw, nullptr, id, detail, snapshot_dir, id_map);
                elements.append(el);
            }
            if (!include_hidden && !isEffectivelyVisible(ci)) continue;
            walker.walk(ci, nullptr);
            if (truncated) return;
        }
    }
}
#endif

// ============================================================================
// serializeElement  —  build a single element's JSON representation
// ============================================================================
QJsonObject UiScanner::serializeElement(QObject* obj,
                                         QObject* parent,
                                         uint64_t id,
                                         const QString& detail,
                                         const QString& snapshot_dir,
                                         const QHash<QObject*, uint64_t>& id_map)
{
    QJsonObject el;

    // -- identity -----------------------------------------------------------
    el[QStringLiteral("id")]   = static_cast<qint64>(id);
    el[QStringLiteral("type")] = QString::fromUtf8(obj->metaObject()->className());

    // -- text ---------------------------------------------------------------
    QString text = displayText(obj);
    if (!text.isEmpty())
        el[QStringLiteral("text")] = text;

    // -- geometry -----------------------------------------------------------
    el[QStringLiteral("rect")]        = rectToJson(obj);
    el[QStringLiteral("global_rect")] = globalRectToJson(obj);

    // -- ordering -----------------------------------------------------------
    el[QStringLiteral("z_order")] = getZOrder(obj);

    // -- parent relationships -----------------------------------------------
    // "parent_id" is the visual parent in the UI tree.
    if (parent) {
        auto it = id_map.find(parent);
        if (it != id_map.end())
            el[QStringLiteral("parent_id")] = static_cast<qint64>(it.value());
        else
            el[QStringLiteral("parent_id")] = 0;
    } else {
        el[QStringLiteral("parent_id")] = 0;
    }

    // "object_parent_id" is the QObject::parent().
    QObject* objParent = obj->parent();
    if (objParent) {
        auto it = id_map.find(objParent);
        if (it != id_map.end())
            el[QStringLiteral("object_parent_id")] = static_cast<qint64>(it.value());
        else
            el[QStringLiteral("object_parent_id")] = 0;
    } else {
        el[QStringLiteral("object_parent_id")] = 0;
    }

    // -- containing window --------------------------------------------------
    QObject* win = getContainingWindow(obj);
    if (win) {
        auto it = id_map.find(win);
        if (it != id_map.end())
            el[QStringLiteral("window_id")] = static_cast<qint64>(it.value());

        // Window title
        if (auto* w = qobject_cast<QWidget*>(win)) {
            QString wt = w->windowTitle();
            if (!wt.isEmpty())
                el[QStringLiteral("window_title")] = wt;
#ifdef QT_COMMANDER_WITH_QML
        } else if (auto* qw = qobject_cast<QQuickWindow*>(win)) {
            QString t = qw->title();
            if (!t.isEmpty())
                el[QStringLiteral("window_title")] = t;
#endif
        }
    } else {
        el[QStringLiteral("window_id")] = 0;
    }

    // child_indices is filled later by the caller.
    el[QStringLiteral("child_indices")] = QJsonArray();

    // -- properties ---------------------------------------------------------
    if (detail != QStringLiteral("core")) {
        el[QStringLiteral("properties")] = serializeProperties(obj, id,
                                                                 detail,
                                                                 snapshot_dir);
    }

    return el;
}

// ============================================================================
// serializeProperties  —  extract Q_PROPERTY and dynamic properties
// ============================================================================
QJsonObject UiScanner::serializeProperties(QObject* obj,
                                            uint64_t element_id,
                                            const QString& detail,
                                            const QString& snapshot_dir)
{
    QJsonObject props;

    const QMetaObject* mo = obj->metaObject();
    if (!mo)
        return props;

    // --- Q_PROPERTY values ---
    for (int i = 0; i < mo->propertyCount(); ++i) {
        QMetaProperty prop = mo->property(i);

        // Skip properties that were already handled as core fields.
        // (They're still useful in "extended" context, so we don't skip.
        //  The spec says "extended" includes ALL Q_PROPERTY values.)

        // Skip QObject* pointer properties (not serializable).
        if (QString(prop.typeName()) == QStringLiteral("QObject*"))
            continue;

        // Skip std::function / functor types by checking the type name.
        const char* typeName = prop.typeName();
        if (!typeName || typeName[0] == '\0')
            continue;
        if (std::strstr(typeName, "std::function") != nullptr)
            continue;
        if (std::strstr(typeName, "QFunctionPointer") != nullptr)
            continue;

        // Skip properties that fail to read.
        if (!prop.isReadable())
            continue;

        QVariant val = prop.read(obj);
        if (!val.isValid())
            continue;

        // --- "core" tier: only visible, enabled, objectName, windowTitle ---
        if (detail == QStringLiteral("core")) {
            const QByteArray pName = prop.name();
            if (pName == "visible" || pName == "enabled" ||
                pName == "objectName" || pName == "windowTitle" ||
                pName == "title")
            {
                QJsonValue jv = propertyToJson(val, detail, snapshot_dir);
                if (!jv.isNull())
                    props[QString::fromUtf8(pName)] = jv;
            }
            continue;
        }

        // --- "extended" / "full": all Q_PROPERTY values ---
        const QByteArray propName = prop.name();
        QString propKey = QString::fromUtf8(propName);

        // Binary property handling for "full" detail.
        if (detail == QStringLiteral("full") &&
            (val.userType() == QMetaType::QPixmap ||
             val.userType() == QMetaType::QImage))
        {
            QString dir = snapshot_dir + QStringLiteral("/props");
            QDir().mkpath(dir);
            QString filePath = dir + QStringLiteral("/") +
                               QString::number(element_id) + QStringLiteral("_") +
                               propKey + QStringLiteral(".png");

            bool saved = false;
            if (val.userType() == QMetaType::QImage) {
                saved = val.value<QImage>().save(filePath, "PNG");
            } else {
                saved = val.value<QPixmap>().save(filePath, "PNG");
            }

            if (saved) {
                props[propKey] = QUrl::fromLocalFile(filePath).toString();
            } else {
                props[propKey] = QStringLiteral("$binary");
            }
            continue;
        }

        // Handle enum properties: include both integer and string name.
        if (prop.isEnumType()) {
            QMetaEnum me = prop.enumerator();
            QJsonObject enumObj;
            enumObj[QStringLiteral("value")] = val.toInt();
            const char* key = me.valueToKey(val.toInt());
            if (key)
                enumObj[QStringLiteral("name")] = QString::fromUtf8(key);
            props[propKey] = enumObj;
            continue;
        }

        // Normal property value.
        QJsonValue jv = propertyToJson(val, detail, snapshot_dir);
        if (!jv.isNull())
            props[propKey] = jv;
    }

    // --- Dynamic properties (attached at runtime via setProperty) ---
    const QList<QByteArray> dynProps = obj->dynamicPropertyNames();
    for (const QByteArray& dynName : dynProps) {
        // Skip properties already handled above.
        if (mo->indexOfProperty(dynName.constData()) >= 0)
            continue;

        QString propKey = QString::fromUtf8(dynName);
        QVariant val = obj->property(dynName.constData());
        if (!val.isValid())
            continue;

        // Skip QObject* types.
        if (val.userType() == QMetaType::QObjectStar)
            continue;

        QJsonValue jv = propertyToJson(val, detail, snapshot_dir);
        if (!jv.isNull())
            props[propKey] = jv;
    }

    return props;
}

// ============================================================================
// propertyToJson  —  convert a QVariant to a QJsonValue
// ============================================================================
QJsonValue UiScanner::propertyToJson(const QVariant& value,
                                      const QString& detail,
                                      const QString& /*snapshot_dir*/)
{
    if (!value.isValid())
        return QJsonValue::Null;

    int typeId = value.userType();

    // --- Primitive / numeric types ---
    if (typeId == QMetaType::Bool)
        return value.toBool();

    if (typeId == QMetaType::Int || typeId == QMetaType::LongLong)
        return value.toLongLong();

    if (typeId == QMetaType::UInt || typeId == QMetaType::ULongLong) {
        quint64 uv = value.toULongLong();
        // JSON numbers are double-precision; large values lose precision.
        if (uv > 9007199254740992ULL)
            return QString::number(uv);
        return static_cast<qint64>(uv);
    }

    if (typeId == QMetaType::Double)
        return value.toDouble();

    if (typeId == QMetaType::Float)
        return static_cast<double>(value.toFloat());

    // --- String / bytearray ---
    if (typeId == QMetaType::QString)
        return value.toString();

    if (typeId == QMetaType::QByteArray) {
        const QByteArray ba = value.toByteArray();
        // If it appears to be a small text blob, return as string.
        // Otherwise base64-encode for "extended"; for "full" the caller
        // handles binary types before calling us.
        return QString::fromUtf8(ba.toBase64());
    }

    // --- Geometry types ---
    if (typeId == QMetaType::QPoint) {
        QPoint p = value.toPoint();
        QJsonObject o;
        o[QStringLiteral("x")] = p.x();
        o[QStringLiteral("y")] = p.y();
        return o;
    }
    if (typeId == QMetaType::QPointF) {
        QPointF p = value.toPointF();
        QJsonObject o;
        o[QStringLiteral("x")] = p.x();
        o[QStringLiteral("y")] = p.y();
        return o;
    }
    if (typeId == QMetaType::QSize) {
        QSize s = value.toSize();
        QJsonObject o;
        o[QStringLiteral("width")]  = s.width();
        o[QStringLiteral("height")] = s.height();
        return o;
    }
    if (typeId == QMetaType::QSizeF) {
        QSizeF s = value.toSizeF();
        QJsonObject o;
        o[QStringLiteral("width")]  = s.width();
        o[QStringLiteral("height")] = s.height();
        return o;
    }
    if (typeId == QMetaType::QRect) {
        QRect r = value.toRect();
        QJsonObject o;
        o[QStringLiteral("x")]      = r.x();
        o[QStringLiteral("y")]      = r.y();
        o[QStringLiteral("width")]  = r.width();
        o[QStringLiteral("height")] = r.height();
        return o;
    }
    if (typeId == QMetaType::QRectF) {
        QRectF r = value.toRectF();
        QJsonObject o;
        o[QStringLiteral("x")]      = r.x();
        o[QStringLiteral("y")]      = r.y();
        o[QStringLiteral("width")]  = r.width();
        o[QStringLiteral("height")] = r.height();
        return o;
    }

    // --- Color ---
    if (typeId == QMetaType::QColor) {
        QColor c = value.value<QColor>();
#ifdef QT_COMMANDER_QT6
        return c.name(QColor::HexArgb);
#else
        return c.name();
#endif
    }

    // --- Font ---
    if (typeId == QMetaType::QFont) {
        QFont f = value.value<QFont>();
        QJsonObject o;
        o[QStringLiteral("family")]    = f.family();
        o[QStringLiteral("pointSize")] = f.pointSize();
        o[QStringLiteral("pixelSize")] = f.pixelSize();
        o[QStringLiteral("bold")]      = f.bold();
        o[QStringLiteral("italic")]    = f.italic();
        o[QStringLiteral("underline")] = f.underline();
        o[QStringLiteral("strikeOut")] = f.strikeOut();
        o[QStringLiteral("weight")]    = f.weight();
        return o;
    }

    // --- Date / Time ---
    if (typeId == QMetaType::QDate)
        return value.toDate().toString(Qt::ISODate);
    if (typeId == QMetaType::QTime)
        return value.toTime().toString(Qt::ISODate);
    if (typeId == QMetaType::QDateTime)
        return value.toDateTime().toString(Qt::ISODate);

    // --- URL ---
    if (typeId == QMetaType::QUrl)
        return value.toUrl().toString();

    // --- Container types ---
    if (typeId == QMetaType::QVariantList) {
        QJsonArray arr;
        const QVariantList list = value.toList();
        for (const QVariant& v : list)
            arr.append(propertyToJson(v, detail, QString()));
        return arr;
    }

    if (typeId == QMetaType::QVariantMap) {
        QJsonObject o;
        const QVariantMap map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            o[it.key()] = propertyToJson(it.value(), detail, QString());
        return o;
    }

    if (typeId == QMetaType::QVariantHash) {
        QJsonObject o;
        const QVariantHash hash = value.toHash();
        for (auto it = hash.begin(); it != hash.end(); ++it)
            o[it.key()] = propertyToJson(it.value(), detail, QString());
        return o;
    }

    if (typeId == QMetaType::QStringList) {
        QJsonArray arr;
        for (const QString& s : value.toStringList())
            arr.append(s);
        return arr;
    }

    // --- Pixmap / Image (handle in "extended" mode as $binary marker) ---
    if (typeId == QMetaType::QPixmap || typeId == QMetaType::QImage) {
        return QStringLiteral("$binary");
    }

    // --- Skip pointer types ---
    if (typeId == QMetaType::QObjectStar)
        return QJsonValue::Null;

    // --- Fallback: try string conversion ---
    QString s = value.toString();
    if (!s.isEmpty())
        return s;

    return QJsonValue::Null;
}

// ============================================================================
// rectToJson  —  element-local coordinates
// ============================================================================
QJsonObject UiScanner::rectToJson(QObject* obj)
{
    // Default: zero rect.
    QJsonObject r;
    r[QStringLiteral("x")]      = 0;
    r[QStringLiteral("y")]      = 0;
    r[QStringLiteral("width")]  = 0;
    r[QStringLiteral("height")] = 0;

    if (auto* w = qobject_cast<QWidget*>(obj)) {
        QRect geo = w->geometry();
        r[QStringLiteral("x")]      = geo.x();
        r[QStringLiteral("y")]      = geo.y();
        r[QStringLiteral("width")]  = geo.width();
        r[QStringLiteral("height")] = geo.height();
#ifdef QT_COMMANDER_WITH_QML
    } else if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        r[QStringLiteral("x")]      = item->x();
        r[QStringLiteral("y")]      = item->y();
        r[QStringLiteral("width")]  = item->width();
        r[QStringLiteral("height")] = item->height();
#endif
    } else if (auto* win = qobject_cast<QWindow*>(obj)) {
        r[QStringLiteral("x")]      = 0;
        r[QStringLiteral("y")]      = 0;
        r[QStringLiteral("width")]  = win->width();
        r[QStringLiteral("height")] = win->height();
    }

    return r;
}

// ============================================================================
// globalRectToJson  —  screen-absolute coordinates
// ============================================================================
QJsonObject UiScanner::globalRectToJson(QObject* obj)
{
    QJsonObject r;
    r[QStringLiteral("x")]      = 0;
    r[QStringLiteral("y")]      = 0;
    r[QStringLiteral("width")]  = 0;
    r[QStringLiteral("height")] = 0;

    if (auto* w = qobject_cast<QWidget*>(obj)) {
        QPoint topLeft = w->mapToGlobal(QPoint(0, 0));
        QSize  sz      = w->geometry().size();
        r[QStringLiteral("x")]      = topLeft.x();
        r[QStringLiteral("y")]      = topLeft.y();
        r[QStringLiteral("width")]  = sz.width();
        r[QStringLiteral("height")] = sz.height();
#ifdef QT_COMMANDER_WITH_QML
    } else if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        QQuickWindow* win = item->window();
        if (!win)
            return r;

        QPointF sceneTL = item->mapToScene(QPointF(0, 0));
        QPointF sceneBR = item->mapToScene(QPointF(item->width(), item->height()));
        QPoint globalTL = win->mapToGlobal(QPoint(qRound(sceneTL.x()), qRound(sceneTL.y())));
        QPoint globalBR = win->mapToGlobal(QPoint(qRound(sceneBR.x()), qRound(sceneBR.y())));

        r[QStringLiteral("x")]      = globalTL.x();
        r[QStringLiteral("y")]      = globalTL.y();
        r[QStringLiteral("width")]  = globalBR.x() - globalTL.x();
        r[QStringLiteral("height")] = globalBR.y() - globalTL.y();
#endif
    } else if (auto* win = qobject_cast<QWindow*>(obj)) {
        r[QStringLiteral("x")]      = win->x();
        r[QStringLiteral("y")]      = win->y();
        r[QStringLiteral("width")]  = win->width();
        r[QStringLiteral("height")] = win->height();
    }

    return r;
}

// ============================================================================
// displayText  —  best-effort human-readable text
// ============================================================================
QString UiScanner::displayText(QObject* obj)
{
    if (!obj)
        return {};

    // Top-level widget window title.
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        if (w->isWindow()) {
            QString wt = w->windowTitle();
            if (!wt.isEmpty())
                return wt;
        }
    }

    // QWindow title.
    if (auto* qw = qobject_cast<QWindow*>(obj)) {
        QString t = qw->title();
        if (!t.isEmpty())
            return t;
    }

    // Generic "text" property (QLabel, QAbstractButton, QLineEdit, QQuickItem…).
    QVariant v = obj->property("text");
    if (v.isValid() && v.userType() == QMetaType::QString) {
        QString t = v.toString().trimmed();
        if (!t.isEmpty())
            return t;
    }

    // "title" property (QGroupBox, QQuickItem with title…).
    v = obj->property("title");
    if (v.isValid() && v.userType() == QMetaType::QString) {
        QString t = v.toString().trimmed();
        if (!t.isEmpty())
            return t;
    }

    // "placeholderText" property.
    v = obj->property("placeholderText");
    if (v.isValid() && v.userType() == QMetaType::QString) {
        QString t = v.toString().trimmed();
        if (!t.isEmpty())
            return t;
    }

    // "currentText" property (QComboBox, etc.).
    v = obj->property("currentText");
    if (v.isValid() && v.userType() == QMetaType::QString) {
        QString t = v.toString().trimmed();
        if (!t.isEmpty())
            return t;
    }

    // objectName fallback.
    QString name = obj->objectName();
    if (!name.isEmpty())
        return name;

    return {};
}

// ============================================================================
// isEffectivelyVisible
// ============================================================================
bool UiScanner::isEffectivelyVisible(QObject* obj)
{
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        // QWidget::isVisible() already walks the ancestor chain.
        return w->isVisible();
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        // QQuickItem::isVisible() only checks the item itself;
        // we must walk parentItem() chain manually.
        const QQuickItem* cur = item;
        while (cur) {
            if (!cur->isVisible())
                return false;
            cur = cur->parentItem();
        }
        return true;
    }
#endif

    if (auto* win = qobject_cast<QWindow*>(obj))
        return win->isVisible();

    // Unknown types default to visible.
    return true;
}

// ============================================================================
// getVisualParent
// ============================================================================
QObject* UiScanner::getVisualParent(QObject* obj)
{
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        // QWidget's visual parent is parentWidget(), not just parent().
        QWidget* pw = w->parentWidget();
        if (pw)
            return pw;

        // For embedded QWidget (no parentWidget), check QObject parent.
        QObject* p = w->parent();
        if (p && p->isWidgetType())
            return p;
        return nullptr;
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(obj))
        return item->parentItem();
#endif

    // Fallback: QObject parent.
    return obj->parent();
}

// ============================================================================
// getZOrder  —  index in visual parent's child list
// ============================================================================
int UiScanner::getZOrder(QObject* obj)
{
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        QWidget* pw = w->parentWidget();
        if (!pw)
            return 0;

        const QObjectList& siblings = pw->children();
        for (int i = 0; i < siblings.size(); ++i) {
            if (siblings[i] == w)
                return i;
        }
        return 0;
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        QQuickItem* pi = item->parentItem();
        if (!pi)
            return 0;

        const auto& siblings = pi->childItems();
        for (int i = 0; i < siblings.size(); ++i) {
            if (siblings[i] == item)
                return i;
        }
        return 0;
    }
#endif

    return 0;
}

// ============================================================================
// getContainingWindow
// ============================================================================
QObject* UiScanner::getContainingWindow(QObject* obj)
{
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        QWidget* win = w->window();    // Handles QWidget::window().
        if (win && win != w)
            return win;
        return w;   // Already a window.
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        QQuickWindow* win = item->window();
        return win;
    }
#endif

    if (auto* win = qobject_cast<QWindow*>(obj))
        return win;

    // Walk QObject parent chain and look for a window.
    QObject* p = obj->parent();
    while (p) {
        if (qobject_cast<QWindow*>(p) || p->isWidgetType())
            return p;
        p = p->parent();
    }

    return nullptr;
}
