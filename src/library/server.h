#pragma once

#include <QAction>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QVariant>
#include <QSet>
#include <QTcpServer>

#include "protocol.h"

class QTcpSocket;
class QWidget;
class QQuickItem;
class QQuickWidget;
class QQuickWindow;
class QWindow;

namespace qt_commander {

class Server : public QObject {
    Q_OBJECT
public:
    Server(QString token, QString tag, QObject* parent = nullptr);

    bool listen(quint16* port, QString* error);
    QString token() const { return token_; }
    QString sessionTag() const { return tag_; }

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    struct QueryContext {
        QJsonObject where;
        bool hasWhere = false;
        QJsonValue props;
        bool includeHidden = false;
        bool screenCoords = false;
        bool compact = false;
        int maxDepth = -1;
        int matched = 0;
        int total = 0;
        int emitted = 0;
        int hiddenPruned = 0;
        bool tooLarge = false;
        static constexpr int kMaxEmitted = 2000;
    };

    struct VirtualRef {
        QPointer<QWidget> owner;
        QPointer<QWindow> windowOwner;
        QString kind;
        QString className;
        int index = 0;
        QRect rect;
        QJsonObject props;
        QPersistentModelIndex persistent;
        QPointer<QAction> action;
        QPointer<QWidget> page;
        QPointer<QObject> qmlObject;
        int logicalIndex = -1;
    };

    void handle(QTcpSocket* socket, const QJsonObject& message);
    void sendResult(QTcpSocket* socket, const QJsonValue& id,
                    const QJsonObject& result);
    void sendError(QTcpSocket* socket, const QJsonValue& id,
                   const QString& code, const QString& message,
                   const QJsonObject& details = QJsonObject());
    void sendBinaryResult(QTcpSocket* socket, const QJsonValue& id,
                          const QJsonObject& result,
                          const QByteArray& binary);

    QJsonObject windows(const QJsonObject& params);
    QJsonObject query(const QJsonObject& params);
    QJsonObject act(const QJsonObject& params);
    QJsonObject screenshot(const QJsonObject& params, QByteArray* binary);
    QJsonObject quickScreenshot(QQuickWindow* window,
                                const QJsonObject& params,
                                QByteArray* binary,
                                const QRect& crop = QRect());
    QJsonObject get(const QJsonObject& params);
    QJsonObject setProperty(const QJsonObject& params);
    QJsonObject call(const QJsonObject& params);
    QJsonObject methods(const QJsonObject& params);
    QJsonValue returnValueToJson(const QVariant& value);
    QJsonObject items(const QJsonObject& params);
    QJsonObject quickItems(QWindow* window, QQuickItem* root,
                           const QJsonObject& params,
                           const QString& ownerId);
    QJsonObject source(const QJsonObject& params);
    QJsonObject actVirtual(const QJsonObject& params, const QString& id);
    QJsonObject buildQmlNode(QWidget* owner, QQuickItem* item,
                             QueryContext& context, int depth, int* serial,
                             bool* anyMatch);
    QString registerQml(QWidget* owner, QQuickItem* item, int index,
                        const QRect& rect, const QJsonObject& props);
    QJsonObject buildQuickWindowNode(QQuickWindow* window,
                                     QueryContext& context, bool isRoot,
                                     bool* anyMatch);
    QJsonObject buildQuickItemNode(QQuickWindow* window, QQuickItem* item,
                                   QueryContext& context, int depth, int* serial,
                                   bool* anyMatch);
    QString registerQuickQml(QQuickWindow* window, QQuickItem* item, int index,
                             const QRect& rect, const QJsonObject& props);
    QString idForWindow(QWindow* window);
    QWindow* windowForId(const QString& id) const;
    QString registerVirtual(QWidget* owner, const QString& kind,
                            const QString& className, int index,
                            const QRect& rect, const QJsonObject& props,
                            const QPersistentModelIndex& persistent,
                            QAction* action = nullptr,
                            QWidget* page = nullptr,
                            int logicalIndex = -1);
    QJsonObject virtualItem(const QString& id,
                            const QJsonValue& requested = QJsonValue());
    bool virtualRect(const struct VirtualRef& ref, QRect* rect) const;
    bool itemMatches(const QJsonObject& props, const QString& kind,
                     const QString& cls, const QJsonObject& where) const;
    QJsonObject nodeFor(QWidget* widget, const QJsonValue& props);
    QWidget* resolveTarget(const QJsonValue& target, QString* error,
                           int nth = -1, bool includeHidden = false);
    QList<QWidget*> collectMatches(const QJsonObject& where,
                                   bool includeHidden = false) const;
    QString resolveErrorCode() const;
    QWidget* findByWhere(const QJsonObject& where) const;
    QWidget* findIn(QWidget* root, const QJsonObject& where) const;
    QJsonObject buildNode(QWidget* widget, QueryContext& context, int depth,
                          bool isRoot, bool* anyMatch);
    void countNode(QWidget* widget, QueryContext& context, int depth);
    bool matches(QWidget* widget, const QJsonObject& where) const;
    void addProps(QJsonObject* node, QWidget* widget,
                  const QJsonValue& spec) const;
    QString idFor(QWidget* widget);

    QTcpServer server_;
    QString token_;
    QString tag_;
    QHash<QTcpSocket*, FrameParser> parsers_;
    QSet<QTcpSocket*> authenticated_;
    QSet<QTcpSocket*> handling_;
    QHash<QWidget*, QString> ids_;
    QHash<QString, QPointer<QWidget>> idToWidget_;
    QHash<QWindow*, QString> windowIds_;
    QHash<QString, QPointer<QWindow>> idToWindow_;
    QSet<QString> staleIds_;
    QHash<QString, VirtualRef> virtuals_;
    QSet<QWidget*> virtualOwners_;
    QString lastResolveCode_;
    quint64 nextId_ = 1;
};

} // namespace qt_commander

