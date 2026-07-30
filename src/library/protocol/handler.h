#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSemaphore>
#include <functional>

class ElementMap;
class RpcServer;

// Protocol handler: receives dispatched operations from RpcServer on the main thread,
// performs the actual Qt operations, and returns results.
// This runs on the MAIN THREAD (dispatched via Qt::QueuedConnection).
class Handler : public QObject {
    Q_OBJECT
public:
    explicit Handler(RpcServer* server, QObject* parent = nullptr);

    // Called from main thread. Returns result QVariant.
    Q_INVOKABLE QVariant doSnapshot(quint64 elementId, const QString& sessionId,
                                     const QString& detail, bool includeHidden,
                                     const QString& snapshotDir, QSemaphore* sem,
                                     quint64 capturedEpoch);
    Q_INVOKABLE QVariant doFindElement(quint64 elementId, const QJsonObject& query, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doGetProperty(quint64 elementId, const QString& name, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doSetProperty(quint64 elementId, const QString& name, const QVariant& value, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doCallMethod(quint64 elementId, const QString& method, const QJsonArray& args, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doScreenshot(quint64 elementId, const QString& dir, int seq, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMouseClick(quint64 elementId, const QString& button, double x, double y, const QStringList& mods, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMousePress(quint64 elementId, const QString& button, double x, double y, const QStringList& mods, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMouseRelease(quint64 elementId, const QString& button, double x, double y, const QStringList& mods, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMouseDblClick(quint64 elementId, const QString& button, double x, double y, const QStringList& mods, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMouseMove(quint64 elementId, double x, double y, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doMouseWheel(quint64 elementId, double dx, double dy, double x, double y, bool pixel, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doKeyPress(quint64 elementId, const QString& key, const QStringList& mods, const QString& text, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doKeyRelease(quint64 elementId, const QString& key, const QStringList& mods, const QString& text, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doTypeText(quint64 elementId, const QString& text, int intervalMs, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doKeyCombo(quint64 elementId, const QString& keys, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doTouchPress(quint64 elementId, double x, double y, int touchId, double pressure, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doTouchMove(quint64 elementId, double x, double y, int touchId, double pressure, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doTouchRelease(int touchId, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doFocus(quint64 elementId, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doClearFocus(quint64 elementId, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doContextMenu(quint64 elementId, double x, double y, bool hasCoords, QSemaphore* sem, quint64 epoch);
    Q_INVOKABLE QVariant doPing(QSemaphore* sem);

    // Helpers (public for unit testing)
    QObject* validateElement(quint64 elementId, quint64 capturedEpoch, QVariantMap& result);
    void setElementMap(ElementMap* map) { element_map_ = map; }
    static QVariantMap makeError(int code, const QString& message);
    static QVariantMap makeOk();

private:

    RpcServer* server_;
    ElementMap* element_map_;
    int snapshot_seq_ = 0;
    int screenshot_seq_ = 0;
    QString workspace_path_;
    QString session_id_;
};
