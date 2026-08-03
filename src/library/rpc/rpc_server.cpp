// =============================================================================
// rpc_server.cpp
//
// TCP JSON-RPC server running inside the injected Qt library.
// Accepts one connection, authenticates, and dispatches widget operations
// to the main thread via Qt::QueuedConnection.
//
// Implements two entry points:
//   1. RpcServer QObject class   -- for direct use by library consumers
//   2. qt_commander::run_rpc_server() -- free function called by entry_*.cpp
// =============================================================================

#include "rpc/rpc_server.h"
#include "../rpc/parse_utils.h"
#include "api.h"
#include "core/element_map.h"
#include "selector/selector.h"
#include "../core/event_injector.h"
#include "../core/screenshot.h"
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickWindow>
#include <QQuickItem>
#endif
#include "../common/socket_utils.h"
#include "../common/framing.h"
#include "protocol/handler.h"

#include <QApplication>
#include <QWidget>
#include <QWindow>
#include <QJsonArray>
#include <QJsonValue>
#include <QMetaObject>
#include <QMetaProperty>
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

/// Collect a QObject's properties into a JSON object.
static QJsonObject collectProperties(QObject* obj, int propDepth) {
    QJsonObject props;
    const QMetaObject* meta = obj->metaObject();
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
/// Negative depth values mean "unlimited".
static QJsonArray collectChildren(QObject* parent, int maxDepth, int propDepth,
                                  ElementMap* elementMap, uint64_t& nextId,
                                  bool includeHidden) {
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
    // QQuickItem children
    if (QQuickItem* qi = qobject_cast<QQuickItem*>(parent)) {
        const auto& items = qi->childItems();
        for (QQuickItem* item : items)
            childList.append(item);
    }
#endif

    for (QObject* child : childList) {
        // Skip widgets the user cannot see (isVisible() covers the whole
        // ancestor chain; hidden tab pages report false here too).
        if (!includeHidden) {
            if (QWidget* cw = qobject_cast<QWidget*>(child)) {
                if (!cw->isVisible())
                    continue;
            }
        }

        const uint64_t id = nextId++;
        elementMap->insert(id, child);

        QJsonObject node;
        node["className"] = QString::fromLatin1(child->metaObject()->className());
        node["objID"] = static_cast<qint64>(id);
        node["properties"] = collectProperties(child, propDepth);
        node["children"] = collectChildren(child, childDepth, propDepth,
                                           elementMap, nextId, includeHidden);
        arr.append(node);
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
// Port file writer -- atomic via temp-file + rename
// ---------------------------------------------------------------------------
bool writePortFileAtomic(const std::string& path, uint16_t port) {
    const std::string tmpPath = path + ".tmp";
    const std::string portStr = std::to_string(port) + '\n';

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
    auto frame = frame_encode(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
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
        rpc_io::sendJsonError(client_fd, authId, 2001,
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
                    const bool includeHidden =
                        rpcParams[QStringLiteral("include_hidden")].toBool(true);

                    // Look up root object from the current element map (before rebuild)
                    QObject* rootObj = nullptr;
                    if (rootId > 0) {
                        rootObj = elementMap->get(rootId);
                    }

                    QJsonArray nodes;
                    locker.unlock();
                    {
                        QWriteLocker wlock(elementMap->rwLock());
                        elementMap->clear();
                        uint64_t nextId = 1;

                        auto addRoot = [&](QObject* obj) {
                            const uint64_t id = nextId++;
                            elementMap->insert(id, obj);
                            QJsonObject node;
                            node["className"] = QString::fromLatin1(obj->metaObject()->className());
                            node["objID"] = static_cast<qint64>(id);
                            node["properties"] = collectProperties(obj, propDepth);
                            node["children"] = collectChildren(
                                obj, maxDepth, propDepth,
                                elementMap.get(), nextId, includeHidden);
                            nodes.append(node);
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
                    // The dispatch holds the read lock; release it before
                    // taking the write lock (read->write upgrade deadlocks).
                    locker.unlock();
                    {
                        QWriteLocker wlock(elementMap->rwLock());
                        elementMap->clear();
                        uint64_t nextId = 1;
                        auto addRoot = [&](QObject* obj) {
                            elementMap->insert(nextId++, obj);
                            collectChildren(obj, -1, 0,
                                            elementMap.get(), nextId, true);
                        };
                        if (auto* app =
                                qobject_cast<QApplication*>(QCoreApplication::instance())) {
                            for (QWidget* w : app->topLevelWidgets())
                                addRoot(w);
                        }
#ifdef QT_COMMANDER_WITH_QML
                        for (QWindow* win : QGuiApplication::topLevelWindows()) {
                            if (qobject_cast<QQuickWindow*>(win))
                                addRoot(win);
                        }
#endif
                        elementMap->incrementEpoch();
                    }
                    locker.relock();
                    const auto results =
                        ElementSelector::find(query, elementMap->snapshot());
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
                    QObject* obj = elementMap->get(elementId);
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
                        result[QStringLiteral("value")] =
                            QJsonValue::fromVariant(val);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
                    if (!obj) {
                        result[QStringLiteral("ok")] = false;
                        result[QStringLiteral("message")] =
                            QStringLiteral("Element not found");
                    } else {
                        QVariantList varArgs;
                        for (const QJsonValue& v : args)
                            varArgs.append(v.toVariant());
                        // Zero-arg methods (e.g. QPushButton::click()) cannot
                        // be invoked with a Q_ARG(QVariantList, ...) attached
                        // -- the signatures never match.  Invoke without
                        // arguments when the caller passed none.
                        bool callOk = false;
                        if (varArgs.isEmpty()) {
                            callOk = QMetaObject::invokeMethod(
                                obj,
                                methodNameStr.toUtf8().constData(),
                                Qt::DirectConnection);
                        } else {
                            callOk = QMetaObject::invokeMethod(
                                obj,
                                methodNameStr.toUtf8().constData(),
                                Qt::DirectConnection,
                                Q_ARG(QVariantList, varArgs));
                        }
                        result[QStringLiteral("ok")] = callOk;
                        if (!callOk) {
                            result[QStringLiteral("message")] =
                                QStringLiteral(
                                    "callMethod failed");
                        }
                    }
                }
                // ---- focus ----
                else if (opMethod == QStringLiteral("focus")) {
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                // ---- dblClick ----
                else if (opMethod == QStringLiteral("dblClick")) {
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
                    QObject* obj = elementMap->get(elementId);
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
// RpcServer member implementation
// =============================================================================

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
RpcServer::RpcServer(QObject* parent)
    : QObject(parent)
{
    ensureMetaTypes();
}

RpcServer::~RpcServer()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
int RpcServer::start(const InitParams* params)
{
    if (!params)
        return -1;

    // Validate version
    if (params->version != INIT_PARAMS_VERSION)
        return -1;

    // Copy string fields from the POD struct (fixed-size C char arrays)
    // The InitParams contract guarantees null-termination within the buffer.
    auto safeCStr = [](const char* arr, size_t maxBytes) -> std::string {
        size_t len = 0;
        while (len < maxBytes && arr[len] != '\0')
            ++len;
        return std::string(arr, len);
    };
    token_         = safeCStr(params->token, INIT_PARAMS_TOKEN_LEN);
    workspace_path_ = safeCStr(params->workspace_path, INIT_PARAMS_MAX_PATH);
    session_id_    = safeCStr(params->session_id, 12);
    port_file_path_ = safeCStr(params->port_file_path, INIT_PARAMS_MAX_PATH);

    // Create the element map (must exist before Handler construction)
    element_map_ = std::make_unique<ElementMap>();

    // Create the Handler -- lives on the main thread as a child of this
    handler_ = new Handler(this, this);

    // Mark running before starting the socket so the worker thread sees it
    running_.store(true, std::memory_order_release);

    // Create listening socket on loopback with OS-assigned port
    uint16_t port = 0;
    listen_fd_ = tcp_listen_loopback(port);
    if (listen_fd_ == INVALID_SOCK) {
        element_map_.reset();
        handler_ = nullptr;   // QObject parent will delete
        running_.store(false, std::memory_order_release);
        return -1;
    }

    // Enable keepalive (2h idle, 1s interval, 3 probes)
    tcp_set_keepalive(listen_fd_, 7200, 1, 3);

    // Write port file atomically so the MCP server can discover our port
    if (!writePortFileAtomic(port_file_path_, port)) {
        tcp_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
        element_map_.reset();
        handler_ = nullptr;
        running_.store(false, std::memory_order_release);
        return -1;
    }

    // Start the worker thread
    worker_thread_ = std::thread(&RpcServer::workerLoop, this);

    return 0;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void RpcServer::shutdown()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel))
        return; // already stopped or never started

    // Close the listen socket to unblock accept() in the worker thread
    if (listen_fd_ != INVALID_SOCK) {
        tcp_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
    }

    // Close the client socket to unblock recv() in the worker thread
    if (client_fd_ != INVALID_SOCK) {
        tcp_close(client_fd_);
        client_fd_ = INVALID_SOCK;
    }

    // Join the worker thread
    if (worker_thread_.joinable())
        worker_thread_.join();

    // Clean up resources (handler is a QObject child, auto-deleted by parent)
    handler_ = nullptr;
    element_map_.reset();
}

// ---------------------------------------------------------------------------
// workerLoop  --  runs on a dedicated background thread
// ---------------------------------------------------------------------------
void RpcServer::workerLoop()
{
    // Accept one connection (blocks until a client connects or listen_fd_ is
    // closed from shutdown())
    client_fd_ = tcp_accept(listen_fd_);

    // Close the listen socket -- we only serve a single connection
    if (listen_fd_ != INVALID_SOCK) {
        tcp_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
    }

    if (client_fd_ == INVALID_SOCK) {
        // Accept failed (likely shutdown was called)
        running_.store(false, std::memory_order_release);
        return;
    }

    // Set keepalive on the client socket
    tcp_set_keepalive(client_fd_, 7200, 1, 3);

    // Authenticate the connection
    if (!authenticateConnection(client_fd_)) {
        tcp_close(client_fd_);
        client_fd_ = INVALID_SOCK;
        running_.store(false, std::memory_order_release);
        return;
    }

    // Main dispatch loop
    while (running_.load(std::memory_order_acquire)) {
        const std::string payload = rpc_io::readFrame(client_fd_);
        if (payload.empty())
            break; // disconnect or error

        // Parse JSON-RPC request
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(payload), &parseErr);
        if (parseErr.error != QJsonParseError::NoError ||
            !doc.isObject()) {
            rpc_io::sendJsonError(client_fd_, 0, -32700,
                          QStringLiteral("Parse error"));
            continue;
        }

        QJsonObject request = doc.object();

        // Check for notification (no "id" field == no response expected)
        const int rpcId = request[QStringLiteral("id")].toInt(-1);
        if (rpcId < 0) {
            const QString method =
                request[QStringLiteral("method")].toString();
            if (method == QStringLiteral("qt.shutdown")) {
                handleShutdown();
                break;
            }
            continue; // other notifications are silently ignored
        }

        // Process the request and send a response
        const std::string response = processRequest(request);
        if (!response.empty()) {
            if (!rpc_io::sendFrame(client_fd_, response))
                break; // send failure -> disconnect
        }
    }

    // Clean up client socket
    if (client_fd_ != INVALID_SOCK) {
        tcp_close(client_fd_);
        client_fd_ = INVALID_SOCK;
    }

    running_.store(false, std::memory_order_release);

    // Notify that the RPC loop has finished
    emit operationCompleted();
}

// ---------------------------------------------------------------------------
// authenticateConnection  --  5-second receive timeout for auth frame
// ---------------------------------------------------------------------------
bool RpcServer::authenticateConnection(socket_t client)
{
    // Set 5-second receive timeout for the initial auth frame
    tcp_set_recv_timeout(client, 5000);

    const std::string payload = rpc_io::readFrame(client);

    // Restore blocking mode (infinite timeout) for subsequent operations
    tcp_set_recv_timeout(client, 0);

    if (payload.empty())
        return false;

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(payload), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    QJsonObject request = doc.object();
    const int id = request[QStringLiteral("id")].toInt(-1);
    const QString method = request[QStringLiteral("method")].toString();
    const QJsonObject params = request[QStringLiteral("params")].toObject();

    if (id < 0 || method != QStringLiteral("qt.authenticate")) {
        rpc_io::sendJsonError(client, id < 0 ? 0 : id, -32600,
                      QStringLiteral("Expected qt.authenticate"));
        return false;
    }

    // Verify token
    const QString clientToken =
        params[QStringLiteral("token")].toString();
    if (clientToken != QString::fromStdString(token_)) {
        rpc_io::sendJsonError(client, id, 2001,
                      QStringLiteral("Authentication failed: invalid token"));
        return false;
    }

    // Success
    QJsonObject ok;
    ok[QStringLiteral("ok")] = true;
    ok[QStringLiteral("message")] = QStringLiteral("Authenticated");
    return rpc_io::sendJsonResponse(client, id, ok);
}

// ---------------------------------------------------------------------------
// processRequest  --  route a JSON-RPC request to the appropriate handler
// ---------------------------------------------------------------------------
std::string RpcServer::processRequest(const QJsonObject& request)
{
    const int id = request[QStringLiteral("id")].toInt(-1);
    if (id < 0)
        return {}; // notification -- no response

    const QString method  = request[QStringLiteral("method")].toString();
    const QJsonObject params = request[QStringLiteral("params")].toObject();

    // ---- Authentication (should never arrive after auth is done) ----
    if (method == QStringLiteral("qt.authenticate")) {
        QJsonObject result = handleAuthenticate(params);
        QByteArray resp = jsonRpcResponse(id, result);
        return std::string(resp.constData(), resp.size());
    }

    // ---- Shutdown ----
    if (method == QStringLiteral("qt.shutdown")) {
        handleShutdown();
        return {}; // no response for shutdown
    }

    // ---- Tool method routing ----
    QJsonObject result;

    if (method == QStringLiteral("qt.snapshot"))
        result = handleSnapshot(params);
    else if (method == QStringLiteral("qt.findElement"))
        result = handleFindElement(params);
    else if (method == QStringLiteral("qt.getProperty"))
        result = handleGetProperty(params);
    else if (method == QStringLiteral("qt.setProperty"))
        result = handleSetProperty(params);
    else if (method == QStringLiteral("qt.callMethod"))
        result = handleCallMethod(params);
    else if (method == QStringLiteral("qt.screenshot"))
        result = handleScreenshot(params);
    else if (method == QStringLiteral("qt.mouseClick"))
        result = handleMouseClick(params);
    else if (method == QStringLiteral("qt.mousePress"))
        result = handleMousePress(params);
    else if (method == QStringLiteral("qt.mouseRelease"))
        result = handleMouseRelease(params);
    else if (method == QStringLiteral("qt.mouseDblClick"))
        result = handleMouseDblClick(params);
    else if (method == QStringLiteral("qt.mouseMove"))
        result = handleMouseMove(params);
    else if (method == QStringLiteral("qt.mouseWheel"))
        result = handleMouseWheel(params);
    else if (method == QStringLiteral("qt.keyPress"))
        result = handleKeyPress(params);
    else if (method == QStringLiteral("qt.keyRelease"))
        result = handleKeyRelease(params);
    else if (method == QStringLiteral("qt.typeText"))
        result = handleTypeText(params);
    else if (method == QStringLiteral("qt.keyCombo"))
        result = handleKeyCombo(params);
    else if (method == QStringLiteral("qt.focus"))
        result = handleFocus(params);
    else if (method == QStringLiteral("qt.clearFocus"))
        result = handleClearFocus(params);
    else if (method == QStringLiteral("qt.contextMenu"))
        result = handleContextMenu(params);
    else if (method == QStringLiteral("qt.touchPress"))
        result = handleTouchPress(params);
    else if (method == QStringLiteral("qt.touchMove"))
        result = handleTouchMove(params);
    else if (method == QStringLiteral("qt.touchRelease"))
        result = handleTouchRelease(params);
    else if (method == QStringLiteral("qt.ping")) {
        result[QStringLiteral("ok")] = true;
        result[QStringLiteral("message")] = QStringLiteral("pong");
    } else {
        QByteArray err = jsonRpcError(
            id, -32601,
            QStringLiteral("Method not found: ") + method);
        return std::string(err.constData(), err.size());
    }

    QByteArray resp = jsonRpcResponse(id, result);
    return std::string(resp.constData(), resp.size());
}

// ---------------------------------------------------------------------------
// readFrame / sendFrame  (delegate to anonymous-namespace helpers)
// ---------------------------------------------------------------------------
std::string RpcServer::readFrame(socket_t client)
{
    return rpc_io::readFrame(client);
}

bool RpcServer::sendFrame(socket_t client, const std::string& data)
{
    return rpc_io::sendFrame(client, data);
}

// ---------------------------------------------------------------------------
// prepareDispatch  --  extract elementId and capture epoch before dispatch
// ---------------------------------------------------------------------------
RpcServer::DispatchArgs RpcServer::prepareDispatch(const QJsonObject& params)
{
    DispatchArgs args;
    qt_parse_element_id(params, args.elementId);
    args.operationParams = params;

    // Capture the current epoch under the element map's read lock so that
    // when the operation runs on the main thread it can validate
    // that the element map hasn't been rebuilt in the meantime.
    if (element_map_) {
        QReadLocker locker(element_map_->rwLock());
        args.capturedEpoch = element_map_->epoch();
    }

    return args;
}

// ---------------------------------------------------------------------------
// dispatchToMain  --  queue work on main thread, wait with 30s timeout
//
// Builds a dispatch envelope (method + params + elementId + capturedEpoch),
// invokes the appropriate Handler::doXxx method on the main thread via a
// lambda queued with Qt::QueuedConnection, and waits for the result on
// a QSemaphore with a 30-second timeout.
// ---------------------------------------------------------------------------
QVariant RpcServer::dispatchToMain(const std::string& method,
                                   const QJsonObject& params)
{
    DispatchArgs args = prepareDispatch(params);

    struct SharedState {
        QVariant result;
        QSemaphore sem;
    };
    auto state = std::make_shared<SharedState>();

    // Full method name including "qt." prefix (matching Handler dispatch)
    const QString methodName = QString::fromStdString(method);

    // Queue a lambda on the main thread.
    // The Handler::doXxx methods take a QSemaphore* which they release
    // internally; we provide a temporary semaphore for that and use
    // our own state->sem for actual synchronization.
    QMetaObject::invokeMethod(
        this,
        [this, state, methodName, args]() {
            QVariant result;
            QSemaphore tempSem; // released by Handler::doXxx, unused here

            // ---- Snapshot ----
            if (methodName == QStringLiteral("qt.snapshot")) {
                result = handler_->doSnapshot(
                    args.elementId,
                    QString::fromStdString(session_id_),
                    args.operationParams
                        [QStringLiteral("detail")].toString(
                            QStringLiteral("minimal")),
                    args.operationParams
                        [QStringLiteral("includeHidden")].toBool(false),
                    args.operationParams
                        [QStringLiteral("snapshotDir")].toString(),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- findElement ----
            else if (methodName == QStringLiteral("qt.findElement")) {
                result = handler_->doFindElement(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("query")].toObject(),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- getProperty ----
            else if (methodName == QStringLiteral("qt.getProperty")) {
                result = handler_->doGetProperty(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("name")].toString(),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- setProperty ----
            else if (methodName == QStringLiteral("qt.setProperty")) {
                result = handler_->doSetProperty(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("name")].toString(),
                    args.operationParams
                        [QStringLiteral("value")].toVariant(),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- callMethod ----
            else if (methodName == QStringLiteral("qt.callMethod")) {
                result = handler_->doCallMethod(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("method")].toString(),
                    args.operationParams
                        [QStringLiteral("args")].toArray(),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- screenshot ----
            else if (methodName == QStringLiteral("qt.screenshot")) {
                result = handler_->doScreenshot(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("dir")].toString(),
                    args.operationParams
                        [QStringLiteral("seq")].toInt(0),
                    &tempSem,
                    args.capturedEpoch);
            }
            // ---- mouseClick ----
            else if (methodName == QStringLiteral("qt.mouseClick")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doMouseClick(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("button")].toString(
                            QStringLiteral("left")),
                    x, y,
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- mousePress ----
            else if (methodName == QStringLiteral("qt.mousePress")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doMousePress(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("button")].toString(
                            QStringLiteral("left")),
                    x, y,
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- mouseRelease ----
            else if (methodName == QStringLiteral("qt.mouseRelease")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doMouseRelease(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("button")].toString(
                            QStringLiteral("left")),
                    x, y,
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- mouseDblClick ----
            else if (methodName ==
                     QStringLiteral("qt.mouseDblClick")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doMouseDblClick(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("button")].toString(
                            QStringLiteral("left")),
                    x, y,
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- mouseMove ----
            else if (methodName == QStringLiteral("qt.mouseMove")) {
                result = handler_->doMouseMove(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("x")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("y")].toDouble(0.0),
                    &tempSem, args.capturedEpoch);
            }
            // ---- mouseWheel ----
            else if (methodName == QStringLiteral("qt.mouseWheel")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doMouseWheel(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("dx")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("dy")].toDouble(0.0),
                    x, y,
                    args.operationParams
                        [QStringLiteral("pixel")].toBool(false),
                    hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- keyPress ----
            else if (methodName == QStringLiteral("qt.keyPress")) {
                result = handler_->doKeyPress(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("key")].toString(),
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    args.operationParams
                        [QStringLiteral("text")].toString(),
                    &tempSem, args.capturedEpoch);
            }
            // ---- keyRelease ----
            else if (methodName == QStringLiteral("qt.keyRelease")) {
                result = handler_->doKeyRelease(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("key")].toString(),
                    toStringList(args.operationParams
                        [QStringLiteral("modifiers")].toArray()),
                    args.operationParams
                        [QStringLiteral("text")].toString(),
                    &tempSem, args.capturedEpoch);
            }
            // ---- typeText ----
            else if (methodName == QStringLiteral("qt.typeText")) {
                result = handler_->doTypeText(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("text")].toString(),
                    args.operationParams
                        [QStringLiteral("intervalMs")].toInt(10),
                    &tempSem, args.capturedEpoch);
            }
            // ---- keyCombo ----
            else if (methodName == QStringLiteral("qt.keyCombo")) {
                result = handler_->doKeyCombo(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("keys")].toString(),
                    &tempSem, args.capturedEpoch);
            }
            // ---- focus ----
            else if (methodName == QStringLiteral("qt.focus")) {
                result = handler_->doFocus(
                    args.elementId,
                    &tempSem, args.capturedEpoch);
            }
            // ---- clearFocus ----
            else if (methodName ==
                     QStringLiteral("qt.clearFocus")) {
                result = handler_->doClearFocus(
                    args.elementId,
                    &tempSem, args.capturedEpoch);
            }
            // ---- contextMenu ----
            else if (methodName ==
                     QStringLiteral("qt.contextMenu")) {
                double x = args.operationParams
                    [QStringLiteral("x")].toDouble(-1.0);
                double y = args.operationParams
                    [QStringLiteral("y")].toDouble(-1.0);
                bool hasCoords =
                    args.operationParams.contains(
                        QStringLiteral("x")) &&
                    args.operationParams.contains(
                        QStringLiteral("y"));
                result = handler_->doContextMenu(
                    args.elementId, x, y, hasCoords,
                    &tempSem, args.capturedEpoch);
            }
            // ---- touchPress ----
            else if (methodName ==
                     QStringLiteral("qt.touchPress")) {
                result = handler_->doTouchPress(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("x")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("y")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("touchId")].toInt(0),
                    args.operationParams
                        [QStringLiteral("pressure")].toDouble(1.0),
                    &tempSem, args.capturedEpoch);
            }
            // ---- touchMove ----
            else if (methodName ==
                     QStringLiteral("qt.touchMove")) {
                result = handler_->doTouchMove(
                    args.elementId,
                    args.operationParams
                        [QStringLiteral("x")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("y")].toDouble(0.0),
                    args.operationParams
                        [QStringLiteral("touchId")].toInt(0),
                    args.operationParams
                        [QStringLiteral("pressure")].toDouble(1.0),
                    &tempSem, args.capturedEpoch);
            }
            // ---- touchRelease ----
            else if (methodName ==
                     QStringLiteral("qt.touchRelease")) {
                result = handler_->doTouchRelease(
                    args.operationParams
                        [QStringLiteral("touchId")].toInt(0),
                    &tempSem, args.capturedEpoch);
            }
            // ---- ping ----
            else if (methodName == QStringLiteral("qt.ping")) {
                result = handler_->doPing(&tempSem);
            }
            // ---- fallback ----
            else {
                QVariantMap fallback;
                fallback[QStringLiteral("ok")] = false;
                fallback[QStringLiteral("message")] =
                    QStringLiteral("Unsupported method: ") +
                    methodName;
                result = QVariant(fallback);
            }

            state->result = result;
            state->sem.release();
        },
        Qt::QueuedConnection);

    // Wait for completion with 30-second timeout
    if (!state->sem.tryAcquire(1, 30000)) {
        QVariantMap err;
        err[QStringLiteral("ok")] = false;
        QVariantMap detail;
        detail[QStringLiteral("code")] = 2004;
        detail[QStringLiteral("message")] =
            QStringLiteral("Main thread operation timed out");
        err[QStringLiteral("error")] = detail;
        return QVariant(err);
    }

    return state->result;
}

// =============================================================================
// Handler method implementations
//
// Each handler parses params from the JSON-RPC request and delegates to
// dispatchToMain() which runs the actual operation on the main thread
// through the Handler class.
// =============================================================================

QJsonObject RpcServer::handleSnapshot(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.snapshot", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleFindElement(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.findElement", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleGetProperty(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.getProperty", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleSetProperty(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.setProperty", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleCallMethod(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.callMethod", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleScreenshot(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.screenshot", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMouseClick(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mouseClick", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMousePress(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mousePress", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMouseRelease(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mouseRelease", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMouseDblClick(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mouseDblClick", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMouseMove(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mouseMove", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleMouseWheel(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.mouseWheel", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleKeyPress(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.keyPress", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleKeyRelease(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.keyRelease", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleTypeText(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.typeText", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleKeyCombo(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.keyCombo", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleFocus(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.focus", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleClearFocus(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.clearFocus", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleContextMenu(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.contextMenu", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleTouchPress(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.touchPress", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleTouchMove(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.touchMove", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleTouchRelease(const QJsonObject& params)
{
    QVariant result = dispatchToMain("qt.touchRelease", params);
    return QJsonObject::fromVariantMap(result.toMap());
}

QJsonObject RpcServer::handleAuthenticate(const QJsonObject& /*params*/)
{
    // Already authenticated at the connection level
    QJsonObject result;
    result[QStringLiteral("ok")] = true;
    result[QStringLiteral("message")] =
        QStringLiteral("Already authenticated");
    return result;
}

void RpcServer::handleShutdown()
{
    // Signal the worker thread to stop
    running_.store(false, std::memory_order_release);

    // Close sockets to unblock any pending I/O
    if (client_fd_ != INVALID_SOCK) {
        tcp_close(client_fd_);
        client_fd_ = INVALID_SOCK;
    }
    if (listen_fd_ != INVALID_SOCK) {
        tcp_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
    }
}
