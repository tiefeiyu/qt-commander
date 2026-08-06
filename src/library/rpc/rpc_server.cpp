// =============================================================================
// rpc_server.cpp
//
// TCP JSON-RPC server running inside the injected Qt library.
// Accepts one connection, authenticates, and dispatches widget operations
// to the main thread via Qt::QueuedConnection.
// =============================================================================

#include "rpc/rpc_server.h"
#include "../rpc/parse_utils.h"
#include "api.h"
#include "core/element_map.h"
#include "selector/selector.h"
#include "../core/event_injector.h"
#include "../core/screenshot.h"
#include "../core/ui_scanner.h"
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickWindow>
#include <QQuickItem>
#endif
#include "../common/socket_utils.h"
#include "../common/framing.h"

#include <QApplication>
#include <QWidget>
#include <QWindow>
#include <QGraphicsOpacityEffect>
#include <QColor>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QDate>
#include <QTime>
#include <QColor>
#include <QSemaphore>
#include <QScreen>
#include <QPixmap>
#include <QImage>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QReadLocker>
#include <QWriteLocker>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTouchEvent>
#include <QContextMenuEvent>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Platform-specific includes for low-level file I/O (port file)
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

// =============================================================================
// Anonymous-namespace helpers  (everything here is file-local)
// =============================================================================
namespace {

// ---------------------------------------------------------------------------
// Snapshot helpers — tree-building with depth control
// ---------------------------------------------------------------------------

/// Serialize a QVariant up to *propDepth* levels for QObject sub-properties.
static QJsonValue serializeValue(const QVariant& value, int propDepth) {
    if (!value.isValid() || value.isNull())
        return QJsonValue();

    switch (static_cast<int>(value.type())) {
    case QVariant::Bool:
        return value.toBool();
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
        return static_cast<double>(value.toLongLong());  // JSON numbers are doubles
    case QVariant::Double:
        return value.toDouble();
    case QVariant::String:
        return value.toString();
    case QVariant::Rect:
    case QVariant::RectF: {
        QRect r = value.toRect();
        return QStringLiteral("%1,%2 %3x%4")
            .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    }
    case QVariant::Point:
    case QVariant::PointF: {
        QPoint p = value.toPoint();
        return QStringLiteral("%1,%2").arg(p.x()).arg(p.y());
    }
    case QVariant::Size:
    case QVariant::SizeF: {
        QSize s = value.toSize();
        return QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
    }
    case QVariant::Date: {
        QDate d = value.toDate();
        return d.toString(Qt::ISODate);
    }
    case QVariant::Time: {
        QTime t = value.toTime();
        return t.toString(Qt::ISODate);
    }
    case QVariant::DateTime: {
        QDateTime dt = value.toDateTime();
        return dt.toString(Qt::ISODate);
    }
    case QVariant::Color: {
        QColor c = value.value<QColor>();
        return c.name();
    }
    default:
        break;
    }

    // QObject sub-property (only if depth > 0 or unlimited)
    if (propDepth != 0 && value.canConvert<QObject*>()) {
        QObject* child = value.value<QObject*>();
        if (child) {
            const int childPropDepth = propDepth > 0 ? propDepth - 1 : propDepth;
            QJsonObject subObj;
            const QMetaObject* meta = child->metaObject();
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                QVariant v = prop.read(child);
                QJsonValue serialized = serializeValue(v, childPropDepth);
                if (!serialized.isUndefined())
                    subObj[QString::fromLatin1(prop.name())] = serialized;
            }
            return subObj;
        }
    }

    return QJsonValue();
}

// ---------------------------------------------------------------------------
// validatedElement — resolve an element id and reject unusable targets
// ---------------------------------------------------------------------------
// Ported from the former protocol::Handler: after the object resolves,
// hidden / disabled / zero-size targets are rejected so operations fail
// predictably instead of silently no-op'ing.  (The destroyed-tracking in
// ElementMap already guarantees a resolved pointer is alive.)
QObject* validatedElement(ElementMap* elementMap, uint64_t elementId,
                          QJsonObject& result)
{
    QObject* obj = elementMap->get(elementId);
    if (!obj) {
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("message")] =
            QStringLiteral("Element not found: id=%1").arg(elementId);
        return nullptr;
    }
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        if (!w->isVisible()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("Element is not visible: id=%1").arg(elementId);
            return nullptr;
        }
        if (!w->isEnabled()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("Element is not enabled: id=%1").arg(elementId);
            return nullptr;
        }
        if (w->size().isEmpty()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("Element has zero size: id=%1").arg(elementId);
            return nullptr;
        }
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* qi = qobject_cast<QQuickItem*>(obj)) {
        if (!qi->isVisible()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("QQuickItem is not visible: id=%1").arg(elementId);
            return nullptr;
        }
        if (!qi->isEnabled()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("QQuickItem is not enabled: id=%1").arg(elementId);
            return nullptr;
        }
        if (qi->width() <= 0.0 || qi->height() <= 0.0) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("message")] =
                QStringLiteral("QQuickItem has zero size: id=%1").arg(elementId);
            return nullptr;
        }
    }
#endif
    return obj;
}

// ---------------------------------------------------------------------------
// Method invocation with typed arguments
// ---------------------------------------------------------------------------

/// Invoke *methodName* on *obj*, converting JSON-derived arguments to the
/// method's declared parameter types (int/uint/double/float/bool/QString/
/// QVariant).  Falls back to zero-arg and QVariantList invocations for
/// compatibility.  On failure fills *errorMsg* with a hint.
static bool invokeMethodTyped(QObject* obj, const QString& methodName,
                              const QVariantList& args, QString* errorMsg)
{
    const QMetaObject* mo = obj->metaObject();
    const QByteArray name = methodName.toUtf8();

    // ---- Pass 1: find an overload whose parameter types we can convert to
    // Walk from 0 so inherited slots (e.g. QDial::setValue coming from
    // QAbstractSlider) are found too -- methodOffset only covers methods
    // declared in the concrete class.
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        // >10 arguments would overflow the fixed conversion buffers below;
        // reject such overloads and let the QVariantList fallback handle it.
        if (m.name() != name || m.parameterCount() != args.size() ||
            args.size() > 10)
            continue;

        int      ints[10];      uint     uints[10];
        double   doubles[10];   float    floats[10];
        bool     bools[10];
        QString  strings[10];
        QVariant variants[10];
        // Qt 6.8 changed Q_ARG to return QMetaMethodArgument (a plain
        // struct, no longer derived from QGenericArgument); the templated
        // invokeMethod overloads accept it directly.  Older Qt (5.x/6.0-6.7)
        // still uses QArgument<T> derived from QGenericArgument.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QMetaMethodArgument ga[10];
#else
        QGenericArgument ga[10];
#endif
        bool convOk = true;

        for (int a = 0; a < args.size(); ++a) {
            // Qt 5: parameterType() returns a metatype id; Qt 6 returns a
            // QMetaType object.  Use QMetaType::typeName() so both work.
            const char* tn = QMetaType::typeName(m.parameterType(a));
            const QByteArray t = tn ? QByteArray(tn) : QByteArray();
            bool ok = false;
            if (t == "int") {
                ints[a] = args[a].toInt(&ok);
                ga[a] = Q_ARG(int, ints[a]);
            } else if (t == "uint") {
                uints[a] = args[a].toUInt(&ok);
                ga[a] = Q_ARG(uint, uints[a]);
            } else if (t == "double") {
                doubles[a] = args[a].toDouble(&ok);
                ga[a] = Q_ARG(double, doubles[a]);
            } else if (t == "float") {
                floats[a] = static_cast<float>(args[a].toDouble(&ok));
                ga[a] = Q_ARG(float, floats[a]);
            } else if (t == "bool") {
                bools[a] = args[a].toBool();
                ok = true;
                ga[a] = Q_ARG(bool, bools[a]);
            } else if (t == "QString") {
                strings[a] = args[a].toString();
                ok = true;
                ga[a] = Q_ARG(QString, strings[a]);
            } else if (t == "QVariant") {
                variants[a] = args[a];
                ok = true;
                ga[a] = Q_ARG(QVariant, variants[a]);
            } else {
                convOk = false;  // unsupported parameter type
                break;
            }
            if (!ok) {
                convOk = false;
                break;
            }
        }
        if (!convOk)
            continue;

        bool callOk = false;
        const char* n = name.constData();
        const int nargs = args.size();
        switch (nargs) {
        case 0: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection); break;
        case 1: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0]); break;
        case 2: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1]); break;
        case 3: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2]); break;
        case 4: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3]); break;
        case 5: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4]); break;
        case 6: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4], ga[5]); break;
        case 7: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4], ga[5], ga[6]); break;
        case 8: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4], ga[5], ga[6], ga[7]); break;
        case 9: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4], ga[5], ga[6], ga[7], ga[8]); break;
        case 10: callOk = QMetaObject::invokeMethod(obj, n, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4], ga[5], ga[6], ga[7], ga[8], ga[9]); break;
        default: return false;
        }
        if (callOk)
            return true;
        // Conversion succeeded but the call failed (method not invokable,
        // e.g. a plain public method) -- try the next overload.
    }

    // ---- Pass 2: legacy fallbacks (zero-arg / QVariantList) --------------
    if (args.isEmpty()) {
        if (QMetaObject::invokeMethod(obj, name.constData(),
                                      Qt::DirectConnection))
            return true;
    } else {
        QVariantList varArgs = args;
        if (QMetaObject::invokeMethod(obj, name.constData(),
                                      Qt::DirectConnection,
                                      Q_ARG(QVariantList, varArgs)))
            return true;
    }

    if (errorMsg) {
        *errorMsg = QStringLiteral("no invokable overload of %1(%2)")
            .arg(methodName)
            .arg(QString::number(args.size()));
    }
    return false;
}

/// Collect a QObject's properties into a JSON object.
///
/// detail tiers (same contract the snapshot tool documents):
///   "core"     -- no properties (the first-class fields on the node are
///                 enough: geometry, visibility, text, window info)
///   "extended" -- a whitelist of common interaction-state properties
///                 (text/checked/value/placeholderText/...), cheap to read
///   "full"     -- every Q_PROPERTY (expensive: ~70 reads per widget)
/// The default is "core": reading all properties of every node dominates
/// snapshot cost (seconds of GUI-thread time and megabytes of JSON on
/// large UIs), and most properties are noise for the AI.
static QJsonObject collectProperties(QObject* obj, int propDepth,
                                     const QString& detail) {
    QJsonObject props;
    if (detail == QStringLiteral("core"))
        return props;
    const QMetaObject* meta = obj->metaObject();
    if (detail == QStringLiteral("extended")) {
        // Read the whitelist via QObject::property() instead of walking
        // the static meta-object: QML components declare their properties
        // dynamically (QQmlVMEMetaObject) and those never appear in
        // [propertyOffset, propertyCount), so a static loop would silently
        // drop e.g. `property string text` on a _QMLTYPE_ component
        // (snapshot showed "properties": {} next to a real text).
        static const QByteArray whitelist[] = {
            "text", "checked", "checkState", "enabled", "visible",
            "placeholderText", "currentText", "currentIndex", "value",
            "minimum", "maximum", "singleStep", "readOnly", "echoMode",
            "pressed", "selected", "expanded", "title", "windowTitle",
            "toolTip", "accessibleName", "maxLength", "modified",
        };
        for (const QByteArray& name : whitelist) {
            const QVariant v = obj->property(name.constData());
            if (!v.isValid())
                continue;  // property does not exist on this object
            QJsonValue sv = serializeValue(v, propDepth);
            if (!sv.isUndefined())
                props[QString::fromLatin1(name)] = sv;
        }
        return props;
    }
    // "full": every static Q_PROPERTY.  QML dynamic properties have no
    // public enumeration API; the extended tier covers the common ones.
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QVariant v = prop.read(obj);
        QJsonValue sv = serializeValue(v, propDepth);
        if (!sv.isUndefined())
            props[QString::fromLatin1(prop.name())] = sv;
    }
    return props;
}

/// Recursively collect children into a JSON array, limited by depth.
/// Negative depth values mean "unlimited".  topLevel/topLevelId name the
/// containing top-level window, focusW the once-computed focus widget
/// (passed down the recursion).
static QJsonArray collectChildren(QObject* parent, int maxDepth, int propDepth,
                                  ElementMap* elementMap, uint64_t& nextId,
                                  bool includeHidden,
                                  QObject* topLevel, uint64_t topLevelId,
                                  QWidget* focusW, const QString& detail);

/// Build one snapshot node (top-level window or descendant).  Every node
/// carries the stable identification and geometry the AI needs to plan
/// operations: objectName, local + global rect (logical pixels), z-order,
/// visibility, enabled, text, focus, and the containing top-level window's
/// id / title / activation / devicePixelRatio.  Geometry is the same
/// logical-pixel contract findElement uses (rect = window content area,
/// global_rect = virtual-desktop coordinates); coordinates are computed
/// inside the target process so mixed-DPI screens stay consistent.
static QJsonObject makeNode(QObject* obj, uint64_t id,
                            QObject* topLevel, uint64_t topLevelId,
                            QWidget* focusW,
                            int maxDepth, int propDepth,
                            ElementMap* elementMap, uint64_t& nextId,
                            bool includeHidden, const QString& detail) {
    QJsonObject node;
    node["className"] = QString::fromLatin1(obj->metaObject()->className());
    node["objID"] = static_cast<qint64>(id);
    // Stable identification (QML objectName must be set explicitly; `id` is
    // QML-internal and never visible from C++).
    node["objectName"] = obj->objectName();
    // Geometry: window-local logical rect + virtual-desktop global rect.
    node["rect"] = UiScanner::rectToJson(obj);
    node["global_rect"] = UiScanner::globalRectToJson(obj);
    node["z_order"] = UiScanner::getZOrder(obj);
    // Visibility / interactivity (raw values; the click tools run the real
    // hit test so a click on an occluded element is decided by Qt itself).
    if (auto* w = qobject_cast<QWidget*>(obj)) {
        node["visible"] = w->isVisible();
        node["enabled"] = w->isEnabled();
        // focusWidget() walks the whole focus chain -- compute it once per
        // snapshot (focusW) and compare pointers here (O(1) per node).
        if (focusW)
            node["focused"] = (focusW == w);
        // Own opacity: windowOpacity applies to top-level windows only;
        // QGraphicsOpacityEffect applies to any widget (it is a QObject
        // child, so it never appears in the snapshot tree itself).
        double op = 1.0;
        if (w->isWindow())
            op *= w->windowOpacity();
        if (auto* eff = qobject_cast<QGraphicsOpacityEffect*>(
                w->graphicsEffect()))
            op *= eff->opacity();
        node["opacity"] = op;
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(obj)) {
        node["visible"] = item->isVisible();
        node["enabled"] = item->isEnabled();
        node["opacity"] = item->opacity();
        // clip: true clips this item AND its children to its bounding
        // rect (Flickable/ListView set it by default).
        node["clip"] = item->clip();
        // Rectangle fill alpha (via the generic meta path -- QQuickRectangle
        // is a private class with no public color() accessor).  Absent for
        // non-color items.
        const QVariant color = obj->property("color");
        if (color.isValid() && color.canConvert<QColor>())
            node["color_alpha"] = color.value<QColor>().alphaF();
        if (QQuickWindow* qw = item->window())
            node["focused"] = (qw->activeFocusItem() == item);
    }
    else if (auto* win = qobject_cast<QWindow*>(obj)) {
        // Top-level QML Window / QQuickWindow: whole-window opacity.
        node["opacity"] = win->opacity();
    }
#endif
    // Text (non-empty only, mirrors findElement's "text" semantics).
    const QString text = UiScanner::displayText(obj);
    if (!text.isEmpty())
        node["text"] = text;
    // Containing top-level window.
    node["topLevelId"] = static_cast<qint64>(topLevelId);
    if (topLevel) {
        if (auto* w = qobject_cast<QWidget*>(topLevel)) {
            node["windowTitle"] = w->windowTitle();
            node["isActiveWindow"] = w->isActiveWindow();
            node["dpr"] = w->devicePixelRatio();
        }
#ifdef QT_COMMANDER_WITH_QML
        else if (auto* qw = qobject_cast<QQuickWindow*>(topLevel)) {
            node["windowTitle"] = qw->title();
            node["isActiveWindow"] = qw->isActive();
            node["dpr"] = qw->devicePixelRatio();
        }
#endif
    }
    node["properties"] = collectProperties(obj, propDepth, detail);
    node["children"] = collectChildren(obj, maxDepth, propDepth,
                                       elementMap, nextId, includeHidden,
                                       topLevel, topLevelId, focusW, detail);
    return node;
}

static QJsonArray collectChildren(QObject* parent, int maxDepth, int propDepth,
                                  ElementMap* elementMap, uint64_t& nextId,
                                  bool includeHidden,
                                  QObject* topLevel, uint64_t topLevelId,
                                  QWidget* focusW, const QString& detail) {
    QJsonArray arr;
    if (maxDepth == 0)
        return arr;
    const int childDepth = maxDepth > 0 ? maxDepth - 1 : maxDepth;  // keep negative

    QObjectList childList;
    // QWidget children
    if (QWidget* w = qobject_cast<QWidget*>(parent)) {
        const QObjectList& raw = w->children();
        for (QObject* o : raw) {
            if (qobject_cast<QWidget*>(o) || o->isWidgetType())
                childList.append(o);
        }
    }
#ifdef QT_COMMANDER_WITH_QML
    // QQuickWindow: the scene root is its contentItem()
    if (QQuickWindow* qw = qobject_cast<QQuickWindow*>(parent)) {
        if (QQuickItem* content = qw->contentItem())
            childList.append(content);
    }
    // QQuickItem children
    if (QQuickItem* qi = qobject_cast<QQuickItem*>(parent)) {
        const auto& items = qi->childItems();
        for (QQuickItem* item : items)
            childList.append(item);
    }
#endif

    for (QObject* child : childList) {
        // Skip elements the user cannot see -- this prunes the whole
        // subtree, so hidden tab pages, unopened dialogs and their
        // children never reach the snapshot.
        if (!includeHidden) {
            if (QWidget* cw = qobject_cast<QWidget*>(child)) {
                // QWidget::isVisible() covers the whole ancestor chain.
                if (!cw->isVisible())
                    continue;
            }
#ifdef QT_COMMANDER_WITH_QML
            if (QQuickItem* ci = qobject_cast<QQuickItem*>(child)) {
                // QQuickItem::isVisible() only checks the item itself;
                // isEffectivelyVisible walks the parentItem() chain.
                if (!UiScanner::isEffectivelyVisible(ci))
                    continue;
            }
#endif
        }

        const uint64_t id = nextId++;
        elementMap->insert(id, child);

        arr.append(makeNode(child, id, topLevel, topLevelId, focusW,
                            childDepth, propDepth,
                            elementMap, nextId, includeHidden, detail));
    }
    return arr;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JSON-RPC protocol helpers
// ---------------------------------------------------------------------------
QByteArray jsonRpcResponse(int id, const QJsonObject& result) {
    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")] = id;
    resp[QStringLiteral("result")] = result;
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

QByteArray jsonRpcError(int id, int code, const QString& message) {
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;
    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")] = id;
    resp[QStringLiteral("error")] = error;
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------------------
// Frame-level socket I/O  (named namespace to avoid clashing with
// RpcServer::readFrame / RpcServer::sendFrame member functions)
// ---------------------------------------------------------------------------
namespace rpc_io {

std::string readFrame(socket_t fd) {
    // Read header (4 bytes — big-endian length)
    uint8_t header[FRAME_HEADER_SIZE];
    if (!tcp_recv_all(fd, header, FRAME_HEADER_SIZE))
        return {};

    // Parse 4-byte big-endian payload length (no magic/version in new protocol)

    uint32_t payloadLen = (static_cast<uint32_t>(header[0]) << 24) |
                           (static_cast<uint32_t>(header[1]) << 16) |
                           (static_cast<uint32_t>(header[2]) << 8)  |
                           (static_cast<uint32_t>(header[3]));
    if (payloadLen == 0 || payloadLen > MAX_FRAME_PAYLOAD)
        return {};

    // Read payload
    std::vector<uint8_t> payload(payloadLen);
    if (!tcp_recv_all(fd, payload.data(), payloadLen))
        return {};

    return std::string(reinterpret_cast<const char*>(payload.data()),
                       payloadLen);
}

bool sendFrame(socket_t fd, const std::string& data) {
    // frame_encode throws for payloads over MAX_FRAME_PAYLOAD; the worker
    // thread has no exception handler, so an uncaught throw would call
    // std::terminate and kill the whole target process.  Catch it and
    // report failure (callers treat a failed send as a disconnect).
    std::vector<uint8_t> frame;
    try {
        frame = frame_encode(
            reinterpret_cast<const uint8_t*>(data.data()), data.size());
    } catch (const std::exception&) {
        return false;
    }
    return tcp_send_all(fd, frame.data(), frame.size());
}

bool sendJsonResponse(socket_t fd, int id, const QJsonObject& result) {
    QByteArray json = jsonRpcResponse(id, result);
    return sendFrame(fd, std::string(json.constData(), json.size()));
}

bool sendJsonError(socket_t fd, int id, int code, const QString& msg) {
    QByteArray json = jsonRpcError(id, code, msg);
    return sendFrame(fd, std::string(json.constData(), json.size()));
}

} // namespace rpc_io

// ---------------------------------------------------------------------------
// QJsonArray of strings -> QStringList
// ---------------------------------------------------------------------------
QStringList toStringList(const QJsonArray& arr) {
    QStringList result;
    result.reserve(arr.size());
    for (const QJsonValue& v : arr)
        result.append(v.toString());
    return result;
}

// ---------------------------------------------------------------------------
// Register Qt meta-types once at library load
// ---------------------------------------------------------------------------
bool ensureMetaTypes() {
    static bool registered = false;
    if (!registered) {
        qRegisterMetaType<QSemaphore*>("QSemaphore*");
        registered = true;
    }
    return true;
}

} // anonymous namespace

// =============================================================================
// qt_commander::run_rpc_server  --  free function called by entry_*.cpp
//
// Runs on a detached thread.  Takes ownership of listen_fd.  Accepts one
// connection, authenticates, then dispatches RPC operations until the
// shutdown flag is set or the peer disconnects.
// =============================================================================
namespace qt_commander {
// ---------------------------------------------------------------------------
// Port file writer -- atomic via temp-file + rename
// ---------------------------------------------------------------------------
bool writePortFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmpPath = path + ".tmp";
    const std::string& portStr = content;

#ifdef _WIN32
    int fd = ::_open(tmpPath.c_str(),
                     _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                     _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        // File exists from a prior run; remove and retry
        ::_unlink(tmpPath.c_str());
        fd = ::_open(tmpPath.c_str(),
                     _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                     _S_IREAD | _S_IWRITE);
        if (fd < 0)
            return false;
    }
    int written = ::_write(fd, portStr.data(),
                           static_cast<unsigned>(portStr.size()));
    bool writeOk = (written >= 0 && static_cast<size_t>(written) == portStr.size());
    int cerr = ::_close(fd);
    if (!writeOk || cerr != 0) {
        ::_unlink(tmpPath.c_str());
        return false;
    }
    // Atomic rename (MOVEFILE_REPLACE_EXISTING is atomic on same volume)
    if (!::MoveFileExA(tmpPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING)) {
        ::_unlink(tmpPath.c_str());
        return false;
    }
#else
    int fd = ::open(tmpPath.c_str(),
                    O_CREAT | O_EXCL | O_WRONLY,
                    S_IRUSR | S_IWUSR);
    if (fd < 0) {
        ::unlink(tmpPath.c_str());
        fd = ::open(tmpPath.c_str(),
                    O_CREAT | O_EXCL | O_WRONLY,
                    S_IRUSR | S_IWUSR);
        if (fd < 0)
            return false;
    }
    ssize_t written = ::write(fd, portStr.data(), portStr.size());
    bool writeOk = (written >= 0 && static_cast<size_t>(written) == portStr.size());
    int cerr = ::close(fd);
    if (!writeOk || cerr != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }
#endif
    return true;
}


void run_rpc_server(socket_t listen_fd,
                    std::string port_file_path,
                    std::string session_id,
                    std::string token,
                    std::atomic<bool>& shutdown_flag)
{
    ensureMetaTypes();

    // ---- Accept one connection ------------------------------------------
    socket_t client_fd = tcp_accept(listen_fd);
    tcp_close(listen_fd);                 // no longer needed
    if (client_fd == INVALID_SOCK)
        return;
    if (shutdown_flag.load()) {
        tcp_close(client_fd);
        return;
    }

    // Set keepalive (2h idle, 1s interval, 3 probes)
    tcp_set_keepalive(client_fd, 7200, 1, 3);

    // ---- Element map for main-thread lookups (shared_ptr for lambda safety) ---
    auto elementMap = std::make_shared<ElementMap>();

    // ---- Authenticate (5-second timeout) --------------------------------
    tcp_set_recv_timeout(client_fd, 5000);

    const std::string authPayload = rpc_io::readFrame(client_fd);
    if (authPayload.empty()) {
        tcp_close(client_fd);
        return;
    }

    // Restore blocking mode for subsequent operations
    tcp_set_recv_timeout(client_fd, 0);

    // Parse auth request
    QJsonParseError parseErr;
    QJsonDocument authDoc = QJsonDocument::fromJson(
        QByteArray::fromStdString(authPayload), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !authDoc.isObject()) {
        tcp_close(client_fd);
        return;
    }

    QJsonObject authReq = authDoc.object();
    const int authId = authReq[QStringLiteral("id")].toInt(-1);
    const QString authMethod = authReq[QStringLiteral("method")].toString();

    if (authId < 0 || authMethod != QStringLiteral("qt.authenticate")) {
        QByteArray err = jsonRpcError(
            authId < 0 ? 0 : authId, -32600,
            QStringLiteral("Expected qt.authenticate"));
        rpc_io::sendFrame(client_fd, std::string(err.constData(), err.size()));
        tcp_close(client_fd);
        return;
    }

    // Verify token
    const QJsonObject authParams =
        authReq[QStringLiteral("params")].toObject();
    const QString clientToken =
        authParams[QStringLiteral("token")].toString();
    const QString expectedToken = QString::fromStdString(token);

    if (clientToken != expectedToken) {
        // 2009 = AuthFailedError on the Python side (2001 there means
        // BuildRequiredError -- the codes must not collide).
        rpc_io::sendJsonError(client_fd, authId, 2009,
                      QStringLiteral("Authentication failed: invalid token"));
        tcp_close(client_fd);
        return;
    }

    // Auth success
    {
        QJsonObject ok;
        ok[QStringLiteral("ok")] = true;
        ok[QStringLiteral("message")] = QStringLiteral("Authenticated");
        rpc_io::sendJsonResponse(client_fd, authId, ok);
    }

    // ---- Main RPC dispatch loop (single-connection) --------------------
    while (!shutdown_flag.load()) {
        const std::string payload = rpc_io::readFrame(client_fd);
        if (payload.empty())
            break; // disconnect or error

        // Parse JSON-RPC request
        QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(payload), &parseErr);
        if (parseErr.error != QJsonParseError::NoError ||
            !doc.isObject()) {
            rpc_io::sendJsonError(client_fd, 0, -32700,
                          QStringLiteral("Parse error"));
            continue;
        }

        QJsonObject request = doc.object();
        const int rpcId = request[QStringLiteral("id")].toInt(-1);
        const QString rpcMethod =
            request[QStringLiteral("method")].toString();
        const QJsonObject rpcParams =
            request[QStringLiteral("params")].toObject();
        const bool isNotification = (rpcId < 0);

        // Route shutdown
        if (rpcMethod == QStringLiteral("qt.shutdown"))
            break; // no response

        // Build shared state for main-thread dispatch
        struct SharedState {
            QByteArray response;
            QSemaphore sem;
        };
        auto state = std::make_shared<SharedState>();

        uint64_t elementId = 0;
        qt_parse_element_id(rpcParams, elementId);

        // Extract method name without "qt." prefix for the operation switch
        QString opMethod = rpcMethod;
        if (opMethod.startsWith(QStringLiteral("qt.")))
            opMethod = opMethod.mid(3);

        // Dispatch to main thread via QCoreApplication (lives on main thread)
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [elementMap, state, opMethod, rpcParams, elementId,
             rpcId, isNotification]() {
                // ---- Runs on the MAIN thread ----------------------------
                QReadLocker locker(elementMap->rwLock());
                const uint64_t epoch = elementMap->epoch();

                QJsonObject result;

                // ---- snapshot ----
                if (opMethod == QStringLiteral("snapshot")) {
                    const uint64_t rootId =
                        static_cast<uint64_t>(rpcParams[QStringLiteral("rootId")].toDouble(0));
                    const int maxDepth =
                        rpcParams[QStringLiteral("maxDepth")].toInt(1);
                    const int propDepth =
                        rpcParams[QStringLiteral("propDepth")].toInt(1);
                    // Property tier: "core" (default; no properties --
                    // first-class fields only), "extended" (whitelist), or
                    // "full" (every Q_PROPERTY).
                    const QString detail =
                        rpcParams[QStringLiteral("detail")].toString(
                            QStringLiteral("core"));
                    const bool includeHidden =
                        rpcParams[QStringLiteral("include_hidden")].toBool(true);

                    // Look up root object from the current element map (before rebuild)
                    QObject* rootObj = nullptr;
                    bool rootError = false;
                    if (rootId > 0) {
                        rootObj = elementMap->get(rootId);
                        if (!rootObj) {
                            // A stale root_id (every snapshot/find rebuilds
                            // the map) must not silently fall back to the
                            // whole tree -- the agent would plan against
                            // wrong data.  Report it explicitly instead.
                            // NB: no `return` here -- the dispatch lambda
                            // must fall through to the response send below.
                            result[QStringLiteral("ok")] = false;
                            result[QStringLiteral("message")] =
                                QStringLiteral(
                                    "Element not found: id=%1 (ids expire on "
                                    "every snapshot/find refresh; take a new "
                                    "snapshot to get fresh ids)")
                                    .arg(rootId);
                            rootError = true;
                        }
                    }

                    QJsonArray nodes;
                    if (!rootError) {
                        locker.unlock();
                        {
                            QWriteLocker wlock(elementMap->rwLock());
                            elementMap->clear();
                            uint64_t nextId = 1;

                            // QApplication::focusWidget() walks the whole focus
                            // chain -- compute it once, compare pointers per node.
                            QWidget* focusW = qApp ? qApp->focusWidget() : nullptr;
                            auto addRoot = [&](QObject* obj) {
                                const uint64_t id = nextId++;
                                elementMap->insert(id, obj);
                                nodes.append(makeNode(
                                    obj, id, obj, id, focusW,
                                    maxDepth, propDepth,
                                    elementMap.get(), nextId, includeHidden,
                                    detail));
                            };

                            if (rootObj) {
                                // rootId > 0: snapshot from a specific element
                                addRoot(rootObj);
                            } else {
                                // rootId == 0: snapshot all top-level windows
                                if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
                                    for (QWidget* w : app->topLevelWidgets())
                                        addRoot(w);
                                }
#ifdef QT_COMMANDER_WITH_QML
                                for (QWindow* win : QGuiApplication::topLevelWindows()) {
                                    if (qobject_cast<QQuickWindow*>(win))
                                        addRoot(win);
                                }
#endif
                            }

                            elementMap->incrementEpoch();
                        }
                        locker.relock();
                    }
                    result["rootId"] = static_cast<qint64>(rootId);
                    result["maxDepth"] = maxDepth;
                    result["propDepth"] = propDepth;
                    result["nodes"] = nodes;
                    result["epoch"] = static_cast<qint64>(elementMap->epoch());
                }
                // ---- findElement ----
                else if (opMethod == QStringLiteral("findElement")) {
                    const QJsonObject query =
                        rpcParams[QStringLiteral("query")].toObject();
                    QJsonArray matches;
                    // ElementSelector walks the whole widget tree (not just
                    // top-level windows) and matches the documented query
                    // fields (type, text, object_name, window_title, ...).
                    //
                    // Rebuild the element map first (same id allocation as
                    // a snapshot) so every returned id is usable with the
                    // other operations even if the caller's previous
                    // snapshot did not cover the matched element.
                    //
                    // Prune the rebuild with the query's own constraints:
                    // depth narrows the traversal, ancestor_id/window_id
                    // restrict it to that subtree (the match set of such a
                    // query is a subset of that subtree, so no result is
                    // lost).  Unconstrained queries keep the old full-tree
                    // rebuild.
                    const QString depthStr =
                        query.value(QStringLiteral("depth")).toString();
                    int rebuildDepth = -1;  // unlimited
                    if (depthStr == QLatin1String("exact"))
                        rebuildDepth = 1;
                    else if (depthStr == QLatin1String("shallow"))
                        rebuildDepth = 2;
                    else if (!depthStr.isEmpty()) {
                        rebuildDepth = depthStr.toInt();  // 0 on failure
                        if (rebuildDepth <= 0)
                            rebuildDepth = -1;  // invalid -> unlimited
                    }
                    const uint64_t ancestorId =
                        static_cast<uint64_t>(
                            query.value(QStringLiteral("ancestor_id"))
                                .toDouble(0));
                    const uint64_t windowId =
                        static_cast<uint64_t>(
                            query.value(QStringLiteral("window_id"))
                                .toDouble(0));
                    // Resolve the constrained roots before clearing the map.
                    QObject* ancestorObj =
                        ancestorId > 0 ? elementMap->get(ancestorId) : nullptr;
                    QObject* windowObj =
                        windowId > 0 ? elementMap->get(windowId) : nullptr;

                    // The dispatch holds the read lock; release it before
                    // taking the write lock (read->write upgrade deadlocks).
                    locker.unlock();
                    {
                        QWriteLocker wlock(elementMap->rwLock());
                        elementMap->clear();
                        uint64_t nextId = 1;
                        QWidget* focusW = qApp ? qApp->focusWidget() : nullptr;
                        auto addRoot = [&](QObject* obj) {
                            const uint64_t id = nextId++;
                            elementMap->insert(id, obj);
                            collectChildren(obj, rebuildDepth, 0,
                                            elementMap.get(), nextId, true,
                                            obj, id, focusW,
                                            QStringLiteral("core"));
                        };
                        if (ancestorObj || windowObj) {
                            addRoot(ancestorObj ? ancestorObj : windowObj);
                        } else {
                            if (auto* app = qobject_cast<QApplication*>(
                                    QCoreApplication::instance())) {
                                for (QWidget* w : app->topLevelWidgets())
                                    addRoot(w);
                            }
#ifdef QT_COMMANDER_WITH_QML
                            for (QWindow* win :
                                 QGuiApplication::topLevelWindows()) {
                                if (qobject_cast<QQuickWindow*>(win))
                                    addRoot(win);
                            }
#endif
                        }
                        elementMap->incrementEpoch();
                    }
                    locker.relock();
                    // The rebuild re-allocated ids, so ancestor_id/window_id
                    // from the caller's (pre-rebuild) map no longer resolve.
                    // Re-map them to the new ids; if the object vanished,
                    // drop the constraint (the match set is empty anyway).
                    QJsonObject effectiveQuery = query;
                    if (ancestorObj) {
                        const uint64_t newAnc =
                            elementMap->idFor(ancestorObj);
                        if (newAnc != 0)
                            effectiveQuery[QStringLiteral("ancestor_id")] =
                                static_cast<double>(newAnc);
                        else
                            effectiveQuery.remove(
                                QStringLiteral("ancestor_id"));
                    }
                    if (windowObj) {
                        const uint64_t newWin =
                            elementMap->idFor(windowObj);
                        if (newWin != 0)
                            effectiveQuery[QStringLiteral("window_id")] =
                                static_cast<double>(newWin);
                        else
                            effectiveQuery.remove(
                                QStringLiteral("window_id"));
                    }
                    const auto results = ElementSelector::find(
                        effectiveQuery, elementMap->snapshot());
                    for (const SelectorResult& r : results) {
                        QJsonObject m;
                        m[QStringLiteral("id")] = static_cast<qint64>(r.id);
                        m[QStringLiteral("objectName")] =
                            r.object->objectName();
                        m[QStringLiteral("className")] =
                            QString::fromLatin1(
                                r.object->metaObject()->className());
                        if (QWidget* w = qobject_cast<QWidget*>(r.object)) {
                            m[QStringLiteral("windowTitle")] =
                                w->windowTitle();
                            m[QStringLiteral("visible")] = w->isVisible();
                            m[QStringLiteral("enabled")] = w->isEnabled();
                            m[QStringLiteral("geometry")] =
                                QStringLiteral("%1,%2 %3x%4")
                                    .arg(w->x()).arg(w->y())
                                    .arg(w->width()).arg(w->height());
                        }
                        matches.append(m);
                    }
                    if (matches.isEmpty()) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral(
                                "No matching element found");
                    } else {
                        result[QStringLiteral("ok")] = true;
                        result[QStringLiteral("elements")] = matches;
                        result[QStringLiteral("count")] =
                            matches.size();
                    }
                }
                // ---- getProperty ----
                else if (opMethod == QStringLiteral("getProperty")) {
                    const QString propName =
                        rpcParams[QStringLiteral("name")].toString();
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QVariant val =
                            obj->property(
                                propName.toUtf8().constData());
                        result[QStringLiteral("ok")] = true;
                        // Use the snapshot serializer so common Qt value
                        // types (QRect/QPoint/QSize/...) come back as
                        // readable strings instead of null.
                        result[QStringLiteral("value")] =
                            serializeValue(val, 0);
                        result[QStringLiteral("type")] =
                            QString::fromLatin1(val.typeName());
                    }
                }
                // ---- setProperty ----
                else if (opMethod == QStringLiteral("setProperty")) {
                    const QString propName =
                        rpcParams[QStringLiteral("name")].toString();
                    const QJsonValue propVal =
                        rpcParams[QStringLiteral("value")];
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found");
                    } else {
                        bool ok = obj->setProperty(
                            propName.toUtf8().constData(),
                            propVal.toVariant());
                        result[QStringLiteral("ok")] = ok;
                        if (!ok) {
                            result[QStringLiteral("message")] =
                                QStringLiteral(
                                    "setProperty failed");
                        }
                    }
                }
                // ---- callMethod ----
                else if (opMethod == QStringLiteral("callMethod")) {
                    const QString methodNameStr =
                        rpcParams[QStringLiteral("method")].toString();
                    const QJsonArray args =
                        rpcParams[QStringLiteral("args")].toArray();
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found");
                    } else {
                        QVariantList varArgs;
                        for (const QJsonValue& v : args)
                            varArgs.append(v.toVariant());
                        QString errMsg;
                        const bool callOk =
                            invokeMethodTyped(obj, methodNameStr,
                                              varArgs, &errMsg);
                        result[QStringLiteral("ok")] = callOk;
                        if (!callOk) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("callMethod failed: %1")
                                    .arg(errMsg);
                        }
                    }
                }
                // ---- focus ----
                else if (opMethod == QStringLiteral("focus")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found");
                    } else {
                        QWidget* w = qobject_cast<QWidget*>(obj);
                        if (w) {
                            w->setFocus();
                        } else {
                            QMetaObject::invokeMethod(
                                obj, "setFocus",
                                Qt::DirectConnection);
                        }
                        result[QStringLiteral("ok")] = true;
                    }
                }
                // ---- clearFocus ----
                else if (opMethod == QStringLiteral("clearFocus")) {
                    QWidget* focused =
                        QApplication::focusWidget();
                    if (focused)
                        focused->clearFocus();
                    QApplication::setActiveWindow(nullptr);
                    result[QStringLiteral("ok")] = true;
                }
                // ---- ping ----
                else if (opMethod == QStringLiteral("ping")) {
                    result[QStringLiteral("ok")] = true;
                    result[QStringLiteral("message")] =
                        QStringLiteral("pong");
                    result[QStringLiteral("timestamp")] =
                        QDateTime::currentDateTimeUtc().toString(
                            Qt::ISODateWithMs);
                }
                // ---- click ----
                else if (opMethod == QStringLiteral("click")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString button =
                            rpcParams[QStringLiteral("button")].toString(
                                QStringLiteral("left"));
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseClick(
                                obj, button, x, y, modifiers, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("click failed");
                        }
                    }
                }
                // ---- clickAt (window-local coordinate, real QPA pipeline) ----
                else if (opMethod == QStringLiteral("clickAt")) {
                    const double x =
                        rpcParams[QStringLiteral("x")].toDouble(0);
                    const double y =
                        rpcParams[QStringLiteral("y")].toDouble(0);
                    const QString button =
                        rpcParams[QStringLiteral("button")].toString(
                            QStringLiteral("left"));
                    QStringList modifiers;
                    const QJsonArray modArr =
                        rpcParams[QStringLiteral("modifiers")].toArray();
                    for (const QJsonValue& v : modArr)
                        modifiers.append(v.toString());

                    QWindow* win = nullptr;
                    const uint64_t windowId = static_cast<uint64_t>(
                        rpcParams[QStringLiteral("window_id")].toDouble(0));
                    if (windowId > 0) {
                        QObject* winObj = validatedElement(elementMap.get(), windowId, result);
                        if (!winObj) {
                            result[QStringLiteral("ok")] = false;
                            result[QStringLiteral("message")] =
                                QStringLiteral(
                                    "Window element not found: id=%1")
                                    .arg(windowId);
                        } else {
                            win = EventInjector::resolveWindow(winObj);
                        }
                    } else {
                        win = EventInjector::primaryWindow();
                    }
                    if (win) {
                        result[QStringLiteral("ok")] =
                            EventInjector::mouseClickAt(
                                win, x, y, button, modifiers);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("clickAt failed");
                        }
                    }
                }
                // ---- clickRegion (center of element's region) ----
                else if (opMethod == QStringLiteral("clickRegion")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString button =
                            rpcParams[QStringLiteral("button")].toString(
                                QStringLiteral("left"));
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseClickRegion(
                                obj, button, modifiers);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("clickRegion failed");
                        }
                    }
                }
                // ---- dblClick ----
                else if (opMethod == QStringLiteral("dblClick")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString button =
                            rpcParams[QStringLiteral("button")].toString(
                                QStringLiteral("left"));
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseDblClick(
                                obj, button, x, y, modifiers, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("dblClick failed");
                        }
                    }
                }
                // ---- mousePress ----
                else if (opMethod == QStringLiteral("mousePress")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString button =
                            rpcParams[QStringLiteral("button")].toString(
                                QStringLiteral("left"));
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::mousePress(
                                obj, button, x, y, modifiers, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("mousePress failed");
                        }
                    }
                }
                // ---- mouseRelease ----
                else if (opMethod == QStringLiteral("mouseRelease")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString button =
                            rpcParams[QStringLiteral("button")].toString(
                                QStringLiteral("left"));
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseRelease(
                                obj, button, x, y, modifiers, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("mouseRelease failed");
                        }
                    }
                }
                // ---- mouseMove ----
                else if (opMethod == QStringLiteral("mouseMove")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        double x = rpcParams[QStringLiteral("x")].toDouble(0.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(0.0);

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseMove(obj, x, y);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("mouseMove failed");
                        }
                    }
                }
                // ---- wheel ----
                else if (opMethod == QStringLiteral("wheel")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const double dx =
                            rpcParams[QStringLiteral("dx")].toDouble(0.0);
                        const double dy =
                            rpcParams[QStringLiteral("dy")].toDouble(0.0);
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));
                        const bool pixel =
                            rpcParams[QStringLiteral("pixel")].toBool(false);

                        result[QStringLiteral("ok")] =
                            EventInjector::mouseWheel(
                                obj, dx, dy, x, y, pixel, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("wheel failed");
                        }
                    }
                }
                // ---- keyPress ----
                else if (opMethod == QStringLiteral("keyPress")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj)
                        obj = QApplication::focusWidget();
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("No target element or focused widget");
                    } else {
                        const QString key =
                            rpcParams[QStringLiteral("key")].toString();
                        const QString text =
                            rpcParams[QStringLiteral("text")].toString();
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::keyPress(
                                obj, key, modifiers, text);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("keyPress failed");
                        }
                    }
                }
                // ---- keyRelease ----
                else if (opMethod == QStringLiteral("keyRelease")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj)
                        obj = QApplication::focusWidget();
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("No target element or focused widget");
                    } else {
                        const QString key =
                            rpcParams[QStringLiteral("key")].toString();
                        const QString text =
                            rpcParams[QStringLiteral("text")].toString();
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::keyRelease(
                                obj, key, modifiers, text);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("keyRelease failed");
                        }
                    }
                }
                // ---- typeText ----
                else if (opMethod == QStringLiteral("typeText")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj)
                        obj = QApplication::focusWidget();
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("No target element or focused widget");
                    } else {
                        const QString text =
                            rpcParams[QStringLiteral("text")].toString();
                        const int intervalMs =
                            rpcParams[QStringLiteral("intervalMs")].toInt(10);
                        QStringList modifiers;
                        const QJsonArray modArr =
                            rpcParams[QStringLiteral("modifiers")].toArray();
                        for (const QJsonValue& v : modArr)
                            modifiers.append(v.toString());

                        result[QStringLiteral("ok")] =
                            EventInjector::typeText(
                                obj, text, intervalMs, modifiers);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("typeText failed");
                        }
                    }
                }
                // ---- keyCombo ----
                else if (opMethod == QStringLiteral("keyCombo")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj)
                        obj = QApplication::focusWidget();
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("No target element or focused widget");
                    } else {
                        const QString keys =
                            rpcParams[QStringLiteral("keys")].toString();

                        result[QStringLiteral("ok")] =
                            EventInjector::keyCombo(obj, keys);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("keyCombo failed");
                        }
                    }
                }
                // ---- touchPress ----
                else if (opMethod == QStringLiteral("touchPress")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const double x =
                            rpcParams[QStringLiteral("x")].toDouble(0.0);
                        const double y =
                            rpcParams[QStringLiteral("y")].toDouble(0.0);
                        const int touchId =
                            rpcParams[QStringLiteral("touchId")].toInt(0);
                        const double pressure =
                            rpcParams[QStringLiteral("pressure")].toDouble(1.0);

                        result[QStringLiteral("ok")] =
                            EventInjector::touchPress(
                                obj, x, y, touchId, pressure);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("touchPress failed");
                        }
                    }
                }
                // ---- touchMove ----
                else if (opMethod == QStringLiteral("touchMove")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const double x =
                            rpcParams[QStringLiteral("x")].toDouble(0.0);
                        const double y =
                            rpcParams[QStringLiteral("y")].toDouble(0.0);
                        const int touchId =
                            rpcParams[QStringLiteral("touchId")].toInt(0);
                        const double pressure =
                            rpcParams[QStringLiteral("pressure")].toDouble(1.0);

                        result[QStringLiteral("ok")] =
                            EventInjector::touchMove(
                                obj, x, y, touchId, pressure);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("touchMove failed");
                        }
                    }
                }
                // ---- touchRelease ----
                else if (opMethod == QStringLiteral("touchRelease")) {
                    const int touchId =
                        rpcParams[QStringLiteral("touchId")].toInt(0);

                    result[QStringLiteral("ok")] =
                        EventInjector::touchRelease(touchId);
                    if (!result[QStringLiteral("ok")].toBool()) {
                        result[QStringLiteral("message")] =
                            QStringLiteral("touchRelease failed");
                    }
                }
                // ---- screenshot ----
                else if (opMethod == QStringLiteral("screenshot")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj && elementId == 0) {
                        // element_id == 0 means "entire window": fall back to
                        // the active (or first visible) top-level widget,
                        // mirroring the snapshot rootId==0 path.
                        if (auto* app =
                                qobject_cast<QApplication*>(QCoreApplication::instance())) {
                            QWidget* top = app->activeWindow();
                            if (!top || !top->isVisible()) {
                                const auto widgets = app->topLevelWidgets();
                                for (QWidget* w : widgets) {
                                    if (w->isVisible()) {
                                        top = w;
                                        break;
                                    }
                                }
                            }
                            obj = top;
                        }
#ifdef QT_COMMANDER_WITH_QML
                        // QML apps have no QWidgets; fall back to the first
                        // visible QQuickWindow.
                        if (!obj) {
                            const auto wins =
                                QGuiApplication::topLevelWindows();
                            for (QWindow* win : wins) {
                                if (auto* qw = qobject_cast<QQuickWindow*>(win)) {
                                    if (qw->isVisible()) {
                                        obj = qw;
                                        break;
                                    }
                                }
                            }
                        }
#endif
                    }
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        const QString dir =
                            rpcParams[QStringLiteral("dir")].toString();
                        const int seq =
                            rpcParams[QStringLiteral("seq")].toInt(0);
                        const QString filePath =
                            Screenshot::capture(obj, dir, seq);
                        if (filePath.isEmpty()) {
                            result[QStringLiteral("ok")] = false;
                            result[QStringLiteral("message")] =
                                QStringLiteral("screenshot failed");
                        } else {
                            result[QStringLiteral("ok")] = true;
                            result[QStringLiteral("path")] = filePath;
                            result[QStringLiteral("seq")] = seq;
                        }
                    }
                }
                // ---- contextMenu ----
                else if (opMethod == QStringLiteral("contextMenu")) {
                    QObject* obj = validatedElement(elementMap.get(), elementId, result);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found: id=%1")
                                .arg(elementId);
                    } else {
                        double x = rpcParams[QStringLiteral("x")].toDouble(-1.0);
                        double y = rpcParams[QStringLiteral("y")].toDouble(-1.0);
                        bool hasCoords =
                            rpcParams.contains(QStringLiteral("x")) &&
                            rpcParams.contains(QStringLiteral("y"));

                        result[QStringLiteral("ok")] =
                            EventInjector::contextMenu(
                                obj, x, y, hasCoords);
                        if (!result[QStringLiteral("ok")].toBool()) {
                            result[QStringLiteral("message")] =
                                QStringLiteral("contextMenu failed");
                        }
                    }
                }
                // ---- Unknown method ----
                else {
                    locker.unlock();
                    if (!isNotification) {
                        state->response = jsonRpcError(
                            rpcId, -32601,
                            QStringLiteral("Method not found: ") +
                                opMethod);
                    }
                    state->sem.release();
                    return;
                }

                locker.unlock();

                if (!isNotification)
                    state->response = jsonRpcResponse(rpcId, result);
                state->sem.release();
            },
            Qt::QueuedConnection);

        // Wait for result with 30-second timeout
        if (!state->sem.tryAcquire(1, 30000)) {
            rpc_io::sendJsonError(
                client_fd,
                rpcId < 0 ? 0 : rpcId, 2004,
                QStringLiteral("Main thread operation timed out"));
        } else if (!state->response.isEmpty()) {
            rpc_io::sendFrame(client_fd,
                      std::string(state->response.constData(),
                                  state->response.size()));
        }
        // Notifications: no response sent
    }

    tcp_close(client_fd);
}

} // namespace qt_commander

// =============================================================================
