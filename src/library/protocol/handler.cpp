#include "protocol/handler.h"
#include "core/element_map.h"
#include "core/ui_scanner.h"
#include "core/event_injector.h"
#include "core/screenshot.h"
#include "selector/selector.h"
#include "rpc/rpc_server.h"

#include <QApplication>
#include <QWidget>
#include <QWindow>
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickItem>
#endif
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSemaphore>
#include <QReadWriteLock>
#include <QReadLocker>
#include <QWriteLocker>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScreen>
#include <QPixmap>
#include <QImage>
#include <QUrl>
#include <QDebug>
#include <algorithm>

// ==============================================================
// Anonymous helpers
// ==============================================================
namespace {

// ---- Ensure directory exists ----
bool ensureDir(const QString& path)
{
    QDir dir(path);
    if (dir.exists())
        return true;
    return dir.mkpath(QStringLiteral("."));
}

// ---- Resolve a possibly-relative path against workspace ----
QString resolvePath(const QString& path, const QString& workspacePath)
{
    if (path.isEmpty())
        return workspacePath;
    QFileInfo fi(path);
    if (fi.isAbsolute())
        return path;
    if (workspacePath.isEmpty())
        return QDir::currentPath() + QLatin1Char('/') + path;
    return workspacePath + QLatin1Char('/') + path;
}

} // anonymous namespace

// ==============================================================
// Handler
// ==============================================================

Handler::Handler(RpcServer* server, QObject* parent)
    : QObject(parent)
    , server_(server)
    , element_map_(server ? server->elementMap() : nullptr)
    , workspace_path_(server ? server->workspacePath() : QString())
    , session_id_(server ? server->sessionId() : QString())
{
}

// --------------------------------------------------------------
// makeError / makeOk
// --------------------------------------------------------------
QVariantMap Handler::makeError(int code, const QString& message)
{
    QVariantMap err;
    err[QStringLiteral("ok")] = false;
    QVariantMap detail;
    detail[QStringLiteral("code")] = code;
    detail[QStringLiteral("message")] = message;
    err[QStringLiteral("error")] = detail;
    return err;
}

QVariantMap Handler::makeOk()
{
    QVariantMap ok;
    ok[QStringLiteral("ok")] = true;
    return ok;
}

// --------------------------------------------------------------
// validateElement
// --------------------------------------------------------------
QObject* Handler::validateElement(quint64 elementId, quint64 capturedEpoch,
                                   QVariantMap& result)
{
    if (!element_map_) {
        result = makeError(-1, QStringLiteral("Element map not initialized"));
        return nullptr;
    }

    // Acquire read lock and validate epoch + element
    QReadLocker locker(element_map_->rwLock());

    const quint64 currentEpoch = element_map_->epoch();
    if (currentEpoch != capturedEpoch) {
        result = makeError(-2,
            QStringLiteral("Element map epoch mismatch: capturedEpoch=%1, currentEpoch=%2")
                .arg(capturedEpoch).arg(currentEpoch));
        return nullptr;
    }

    QObject* obj = element_map_->get(elementId);
    if (!obj) {
        result = makeError(-3,
            QStringLiteral("Element not found: id=%1").arg(elementId));
        return nullptr;
    }

    // For QWidget subclasses: check visibility, enabled, and non-zero size
    if (QWidget* w = qobject_cast<QWidget*>(obj)) {
        if (!w->isVisible()) {
            result = makeError(-4,
                QStringLiteral("Element is not visible: id=%1").arg(elementId));
            return nullptr;
        }
        if (!w->isEnabled()) {
            result = makeError(-5,
                QStringLiteral("Element is not enabled: id=%1").arg(elementId));
            return nullptr;
        }
        if (w->size().isEmpty()) {
            result = makeError(-6,
                QStringLiteral("Element has zero size: id=%1").arg(elementId));
            return nullptr;
        }
    }

#ifdef QT_COMMANDER_WITH_QML
    // For QQuickItem subclasses: check similar properties
    if (QQuickItem* qi = qobject_cast<QQuickItem*>(obj)) {
        if (!qi->isVisible()) {
            result = makeError(-7,
                QStringLiteral("QQuickItem is not visible: id=%1").arg(elementId));
            return nullptr;
        }
        if (!qi->isEnabled()) {
            result = makeError(-8,
                QStringLiteral("QQuickItem is not enabled: id=%1").arg(elementId));
            return nullptr;
        }
        if (qi->width() <= 0.0 || qi->height() <= 0.0) {
            result = makeError(-9,
                QStringLiteral("QQuickItem has zero size: id=%1").arg(elementId));
            return nullptr;
        }
    }
#endif

    // Locker releases automatically here
    return obj;
}

// ==============================================================
// doSnapshot -- generates full UI tree snapshot
// ==============================================================
QVariant Handler::doSnapshot(quint64 elementId, const QString& sessionId,
                              const QString& detail, bool includeHidden,
                              const QString& snapshotDir, QSemaphore* sem,
                              quint64 capturedEpoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, capturedEpoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    // Generate the snapshot JSON tree (UiScanner handles element map locking internally)
    QString outDir = resolvePath(snapshotDir, workspace_path_);
    if (!sessionId.isEmpty())
        outDir = outDir + QLatin1Char('/') + sessionId;
    if (!ensureDir(outDir)) {
        result = makeError(-10,
            QStringLiteral("Failed to create snapshot directory: %1").arg(outDir));
        if (sem) sem->release();
        return QVariant(result);
    }

    const int seq = ++snapshot_seq_;
    QJsonObject snapshot = UiScanner::generateSnapshot(
        sessionId.isEmpty() ? session_id_ : sessionId,
        seq, detail, includeHidden, element_map_, outDir);

    // Write the snapshot JSON to file
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmsszzz"));
    const QString filename = QStringLiteral("snapshot_%1_%2.json").arg(seq).arg(ts);
    const QString filePath = outDir + QLatin1Char('/') + filename;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // Non-fatal: snapshot data is still available in the return value
    } else {
        file.write(QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
        file.close();
    }

    result = makeOk();
    result[QStringLiteral("snapshot")] = snapshot;
    result[QStringLiteral("uri")] = QUrl::fromLocalFile(filePath).toString();
    result[QStringLiteral("path")] = filePath;
    result[QStringLiteral("seq")] = seq;

    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doFindElement
// ==============================================================
QVariant Handler::doFindElement(quint64 elementId, const QJsonObject& query,
                                 QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    // Use ElementSelector to find elements matching the query
    auto matches = ElementSelector::find(query, element_map_->snapshot());
    QJsonArray arr;
    for (auto& m : matches) {
        QJsonObject el;
        el[QStringLiteral("element_id")] = static_cast<qint64>(m.id);
        el[QStringLiteral("type")] = QString::fromLatin1(m.object->metaObject()->className());
        arr.append(el);
    }
    result = makeOk();
    result[QStringLiteral("elements")] = arr;

    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doGetProperty
// ==============================================================
QVariant Handler::doGetProperty(quint64 elementId, const QString& name,
                                 QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    QVariant value = obj->property(name.toUtf8().constData());
    result = makeOk();
    result[QStringLiteral("value")] = QJsonValue::fromVariant(value);

    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doSetProperty
// ==============================================================
QVariant Handler::doSetProperty(quint64 elementId, const QString& name,
                                 const QVariant& value, QSemaphore* sem,
                                 quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    bool ok = obj->setProperty(name.toUtf8().constData(), value);
    if (!ok) {
        result = makeError(-12,
            QStringLiteral("Failed to set property '%1' on element %2")
                .arg(name).arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }

    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doCallMethod
// ==============================================================
QVariant Handler::doCallMethod(quint64 elementId, const QString& method,
                                const QJsonArray& args, QSemaphore* sem,
                                quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    QVariant retVal;
    bool invoked = QMetaObject::invokeMethod(obj, method.toUtf8().constData(),
        Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, retVal));
    if (!invoked) {
        if (sem) sem->release();
        return QVariant(makeError(-73, QStringLiteral("callMethod failed on element %1").arg(elementId)));
    }
    result = makeOk();
    result[QStringLiteral("result")] = QJsonValue::fromVariant(retVal);

    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doScreenshot
// ==============================================================
QVariant Handler::doScreenshot(quint64 elementId, const QString& dir,
                                int seq, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }

    const int useSeq = (seq > 0) ? seq : ++screenshot_seq_;
    QString outDir = resolvePath(dir, workspace_path_);

    const QString filePath = Screenshot::capture(obj, outDir, useSeq);
    if (filePath.isEmpty()) {
        result = makeError(-13,
            QStringLiteral("Failed to capture screenshot of element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }

    result = makeOk();
    result[QStringLiteral("path")] = filePath;
    result[QStringLiteral("uri")] = QUrl::fromLocalFile(filePath).toString();
    result[QStringLiteral("seq")] = useSeq;

    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// Mouse event helpers
// ==============================================================
// Each mouse method follows the same pattern:
//   1. validateElement
//   2. resolve coordinate (rel to widget if hasCoords, else center)
//   3. resolve button and modifiers
//   4. call EventInjector
//   5. return result
// ==============================================================

#define HANDLER_MOUSE_IMPL(MethodName, EventInjectorMethod)              \
QVariant Handler::doMouse##MethodName(                                   \
    quint64 elementId, const QString& button, double x, double y,        \
    const QStringList& mods, bool hasCoords, QSemaphore* sem,            \
    quint64 epoch)                                                        \
{                                                                        \
    QVariantMap result;                                                  \
    QObject* obj = validateElement(elementId, epoch, result);            \
    if (!obj) {                                                          \
        if (sem) sem->release();                                         \
        return QVariant(result);                                         \
    }                                                                    \
    bool ok = EventInjector::EventInjectorMethod(                        \
        obj, button, x, y, mods, hasCoords);                             \
    if (!ok) {                                                           \
        result = makeError(-20,                                          \
            QStringLiteral("mouse" #MethodName " failed on element %1")  \
                .arg(elementId));                                        \
        if (sem) sem->release();                                         \
        return QVariant(result);                                         \
    }                                                                    \
    result = makeOk();                                                   \
    if (sem) sem->release();                                             \
    return QVariant(result);                                             \
}

HANDLER_MOUSE_IMPL(Click,    mouseClick)
HANDLER_MOUSE_IMPL(Press,    mousePress)
HANDLER_MOUSE_IMPL(Release,  mouseRelease)
HANDLER_MOUSE_IMPL(DblClick, mouseDblClick)

// --------------------------------------------------------------
// doMouseMove  (no button parameter)
// --------------------------------------------------------------
QVariant Handler::doMouseMove(quint64 elementId, double x, double y,
                               QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::mouseMove(obj, x, y);
    if (!ok) {
        result = makeError(-21,
            QStringLiteral("mouseMove failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// --------------------------------------------------------------
// doMouseWheel
// --------------------------------------------------------------
QVariant Handler::doMouseWheel(quint64 elementId, double dx, double dy,
                                double x, double y, bool pixel, bool hasCoords,
                                QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::mouseWheel(obj, dx, dy, x, y, pixel, hasCoords);
    if (!ok) {
        result = makeError(-22,
            QStringLiteral("mouseWheel failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// Keyboard methods
// ==============================================================

#define HANDLER_KEY_IMPL(MethodName, EventInjectorMethod)                  \
QVariant Handler::do##MethodName(                                          \
    quint64 elementId, const QString& key, const QStringList& mods,        \
    const QString& text, QSemaphore* sem, quint64 epoch)                   \
{                                                                          \
    QVariantMap result;                                                    \
    QObject* obj = validateElement(elementId, epoch, result);              \
    if (!obj) {                                                            \
        if (sem) sem->release();                                           \
        return QVariant(result);                                           \
    }                                                                      \
    bool ok = EventInjector::EventInjectorMethod(obj, key, mods, text);    \
    if (!ok) {                                                             \
        result = makeError(-30,                                            \
            QStringLiteral(#MethodName " failed on element %1")            \
                .arg(elementId));                                          \
        if (sem) sem->release();                                           \
        return QVariant(result);                                           \
    }                                                                      \
    result = makeOk();                                                     \
    if (sem) sem->release();                                               \
    return QVariant(result);                                               \
}

HANDLER_KEY_IMPL(KeyPress,   keyPress)
HANDLER_KEY_IMPL(KeyRelease, keyRelease)

// --------------------------------------------------------------
// doTypeText
// --------------------------------------------------------------
QVariant Handler::doTypeText(quint64 elementId, const QString& text,
                              int intervalMs, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::typeText(obj, text, intervalMs);
    if (!ok) {
        result = makeError(-32,
            QStringLiteral("typeText failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// --------------------------------------------------------------
// doKeyCombo
// --------------------------------------------------------------
QVariant Handler::doKeyCombo(quint64 elementId, const QString& keys,
                              QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::keyCombo(obj, keys);
    if (!ok) {
        result = makeError(-33,
            QStringLiteral("keyCombo failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// Touch methods
// ==============================================================

// ---- doTouchPress ----
QVariant Handler::doTouchPress(quint64 elementId, double x, double y,
                                int touchId, double pressure,
                                QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::touchPress(obj, x, y, touchId, pressure);
    if (!ok) {
        result = makeError(-40,
            QStringLiteral("touchPress failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ---- doTouchMove ----
QVariant Handler::doTouchMove(quint64 elementId, double x, double y,
                               int touchId, double pressure,
                               QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::touchMove(obj, x, y, touchId, pressure);
    if (!ok) {
        result = makeError(-41,
            QStringLiteral("touchMove failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ---- doTouchRelease (no elementId / epoch) ----
QVariant Handler::doTouchRelease(int touchId, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    Q_UNUSED(epoch); // touch release is global, no element validation needed
    bool ok = EventInjector::touchRelease(touchId);
    if (!ok) {
        result = makeError(-42,
            QStringLiteral("touchRelease failed for touchId %1").arg(touchId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// Focus methods
// ==============================================================

// ---- doFocus ----
QVariant Handler::doFocus(quint64 elementId, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::focus(obj);
    if (!ok) {
        result = makeError(-50,
            QStringLiteral("focus failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ---- doClearFocus ----
QVariant Handler::doClearFocus(quint64 elementId, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = this->validateElement(elementId, epoch, result);
    if (!obj) { if (sem) sem->release(); return QVariant(result); }
    bool ok = EventInjector::clearFocus(obj);
    if (!ok) { result = makeError(-51, QStringLiteral("clearFocus failed on element %1").arg(elementId)); if (sem) sem->release(); return QVariant(result); }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// Context menu
// ==============================================================
QVariant Handler::doContextMenu(quint64 elementId, double x, double y,
                                 bool hasCoords, QSemaphore* sem, quint64 epoch)
{
    QVariantMap result;
    QObject* obj = validateElement(elementId, epoch, result);
    if (!obj) {
        if (sem) sem->release();
        return QVariant(result);
    }
    bool ok = EventInjector::contextMenu(obj, x, y, hasCoords);
    if (!ok) {
        result = makeError(-60,
            QStringLiteral("contextMenu failed on element %1").arg(elementId));
        if (sem) sem->release();
        return QVariant(result);
    }
    result = makeOk();
    if (sem) sem->release();
    return QVariant(result);
}

// ==============================================================
// doPing
// ==============================================================
QVariant Handler::doPing(QSemaphore* sem)
{
    QVariantMap result = makeOk();
    result[QStringLiteral("message")] = QStringLiteral("pong");
    result[QStringLiteral("timestamp")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (sem) sem->release();
    return QVariant(result);
}
