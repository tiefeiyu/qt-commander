#include "server.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QDockWidget>
#include <QHeaderView>
#include <QHoverEvent>
#include <QImageWriter>
#include <QJsonArray>
#include <QKeySequence>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListView>
#include <QFont>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMetaMethod>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QMetaProperty>
#include <QMetaType>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStyle>
#include <QStyleOptionGroupBox>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTcpSocket>
#include <QTreeView>
#include <QThread>
#include <QWheelEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QRegularExpression>
#include <QUrl>
#include <QVariant>
#include <QXmlStreamReader>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace qt_commander {

namespace {

QString qmlTypeName(const QQuickItem* item);
QString qmlIdName(const QQuickItem* item);
QRect qmlItemRect(const QQuickItem* item, const QQuickWidget* owner);

constexpr const char* kLibraryVersion = "2.0.0";

bool validMatcher(const QJsonValue& spec)
{
    if (spec.isString())
        return true;
    if (!spec.isObject())
        return false;
    const QJsonObject object = spec.toObject();
    if (object.size() != 1)
        return false;
    static const QSet<QString> operators{
        QStringLiteral("contains"), QStringLiteral("startsWith"),
        QStringLiteral("regex"), QStringLiteral("equals")};
    return operators.contains(object.begin().key());
}

bool matchText(const QJsonValue& spec, const QString& actual)
{
    if (spec.isString()) {
        const QString pattern = spec.toString();
        if (pattern.contains(QLatin1Char('*')) ||
            pattern.contains(QLatin1Char('?'))) {
            return QRegularExpression(
                       QRegularExpression::wildcardToRegularExpression(pattern),
                       QRegularExpression::CaseInsensitiveOption)
                .match(actual)
                .hasMatch();
        }
        return actual == pattern;
    }
    if (!spec.isObject())
        return false;
    const QJsonObject object = spec.toObject();
    if (object.size() != 1)
        return false;
    const auto it = object.begin();
    const QString value = it.value().toString();
    if (it.key() == QStringLiteral("equals"))
        return actual == value;
    if (it.key() == QStringLiteral("contains"))
        return actual.contains(value);
    if (it.key() == QStringLiteral("startsWith"))
        return actual.startsWith(value);
    if (it.key() == QStringLiteral("regex"))
        return QRegularExpression(value).match(actual).hasMatch();
    return false;
}

QJsonObject helloResult(const QString& tag)
{
    QJsonObject limits;
    limits.insert(QStringLiteral("max_inflight"), 8);
    limits.insert(QStringLiteral("max_frame"), kMaxFrameBytes);

    QJsonObject result;
    result.insert(QStringLiteral("protocol"), 2);
    result.insert(QStringLiteral("library_version"),
                  QString::fromLatin1(kLibraryVersion));
    result.insert(QStringLiteral("capabilities"),
                  QJsonArray{QStringLiteral("query"), QStringLiteral("act"),
                             QStringLiteral("screenshot"),
                             QStringLiteral("get"), QStringLiteral("set"),
                             QStringLiteral("call"), QStringLiteral("items"),
                             QStringLiteral("source"), QStringLiteral("windows")});
    result.insert(QStringLiteral("session_tag"), tag);
    result.insert(QStringLiteral("limits"), limits);
    return result;
}

bool isCandidate(QWidget* widget, bool includeHidden)
{
    return widget && (includeHidden || widget->isVisible());
}

bool constantTimeEqual(const QString& left, const QString& right)
{
    const QByteArray a = left.toUtf8();
    const QByteArray b = right.toUtf8();
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

struct UiHit {
    QString file;
    int line = 0;
};

QObject* objectFromVariant(const QVariant& value)
{
    if (!value.isValid())
        return nullptr;
    if (value.userType() == QMetaType::QObjectStar)
        return value.value<QObject*>();
    if (value.constData())
        return *static_cast<QObject* const*>(value.constData());
    return nullptr;
}

QStringList sourceRoots()
{
    QStringList roots;
    const QByteArray configured = qgetenv("QT_COMMANDER_SOURCE_DIR");
    if (!configured.isEmpty())
        roots.append(QString::fromLocal8Bit(configured));
    roots.append(QCoreApplication::applicationDirPath());
    roots.append(QDir::currentPath());
    return roots;
}

bool findUiLocation(const QString& objectName, UiHit* hit)
{
    if (objectName.isEmpty())
        return false;
    const QStringList roots = sourceRoots();
    QSet<QString> seen;
    int scanned = 0;
    for (const QString& root : roots) {
        QDirIterator it(root, {QStringLiteral("*.ui")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && scanned < 500) {
            const QString path = it.next();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || seen.contains(canonical))
                continue;
            seen.insert(canonical);
            ++scanned;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QXmlStreamReader xml(&file);
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement() || xml.name() != QStringLiteral("widget"))
                    continue;
                const QXmlStreamAttributes attributes = xml.attributes();
                if (attributes.value(QStringLiteral("name")).toString() ==
                    objectName) {
                    hit->file = canonical;
                    hit->line = int(xml.lineNumber());
                    return true;
                }
            }
        }
    }
    return false;
}

bool findQmlLocation(const QString& objectName, UiHit* hit)
{
    if (objectName.isEmpty())
        return false;
    const QStringList roots = sourceRoots();
    QSet<QString> seen;
    int scanned = 0;
    for (const QString& root : roots) {
        QDirIterator it(root, {QStringLiteral("*.qml")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && scanned < 500) {
            const QString path = it.next();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || seen.contains(canonical))
                continue;
            seen.insert(canonical);
            ++scanned;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            int line = 0;
            while (!file.atEnd()) {
                const QString text = QString::fromUtf8(file.readLine());
                ++line;
                if (!text.contains(QStringLiteral("objectName")) &&
                    !text.contains(QStringLiteral("id:")))
                    continue;
                if (text.contains(QStringLiteral("\"%1\"").arg(objectName)) ||
                    text.contains(QStringLiteral("'%1'").arg(objectName)) ||
                    text.contains(QStringLiteral("id: %1").arg(objectName)) ||
                    text.contains(QStringLiteral("id:%1").arg(objectName))) {
                    hit->file = canonical;
                    hit->line = line;
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

Server::Server(QString token, QString tag, QObject* parent)
    : QObject(parent), token_(std::move(token)), tag_(std::move(tag))
{
    connect(&server_, &QTcpServer::newConnection,
            this, &Server::onNewConnection);
}

bool Server::listen(quint16* port, QString* error)
{
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
        if (error)
            *error = server_.errorString();
        return false;
    }
    if (port)
        *port = server_.serverPort();
    return true;
}

void Server::onNewConnection()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (!socket)
            break;
        parsers_.insert(socket, FrameParser{});
        connect(socket, &QTcpSocket::readyRead, this, &Server::onReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &Server::onDisconnected);
    }
}

void Server::onReadyRead()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !parsers_.contains(socket) || handling_.contains(socket))
        return;

    handling_.insert(socket);
    FrameParser parser = parsers_.value(socket);
    Frame frame;
    QString error;
    do {
        parser.append(socket->readAll());
        while (parser.next(&frame, &error)) {
            // Save the remaining bytes before dispatch: handle() may enter a
            // nested event loop and onDisconnected() may remove this entry.
            parsers_.insert(socket, parser);
            handle(socket, frame.message);
            if (!parsers_.contains(socket)) {
                handling_.remove(socket);
                return;
            }
        }
        parsers_.insert(socket, parser);
    } while (socket->bytesAvailable() > 0);

    handling_.remove(socket);
    if (!error.isEmpty())
        socket->disconnectFromHost();
}

void Server::onDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    parsers_.remove(socket);
    authenticated_.remove(socket);
    handling_.remove(socket);
    socket->deleteLater();
}

void Server::sendResult(QTcpSocket* socket, const QJsonValue& id,
                          const QJsonObject& result)
{
    QJsonObject response;
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("result"), result);
    socket->write(encodeFrame(response));
}

void Server::sendError(QTcpSocket* socket, const QJsonValue& id,
                         const QString& code, const QString& message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    error.insert(QStringLiteral("retryable"), false);

    QJsonObject response;
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), error);
    socket->write(encodeFrame(response));
}

void Server::sendBinaryResult(QTcpSocket* socket, const QJsonValue& id,
                                const QJsonObject& result,
                                const QByteArray& binary)
{
    QJsonObject response;
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("result"), result);
    socket->write(encodeFrame(response, binary));
}

void Server::handle(QTcpSocket* socket, const QJsonObject& message)
{
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("hello")) {
        if (params.value(QStringLiteral("protocol")).toInt(0) != 2) {
            sendError(socket, id, QStringLiteral("E_PROTOCOL"),
                      QStringLiteral("unsupported protocol"));
            socket->disconnectFromHost();
            return;
        }
        if (!constantTimeEqual(
                params.value(QStringLiteral("token")).toString(), token_)) {
            sendError(socket, id, QStringLiteral("E_AUTH_FAILED"),
                      QStringLiteral("token rejected"));
            socket->disconnectFromHost();
            return;
        }
        authenticated_.insert(socket);
        sendResult(socket, id, helloResult(tag_));
        return;
    }

    if (!authenticated_.contains(socket)) {
        sendError(socket, id, QStringLiteral("E_AUTH_FAILED"),
                  QStringLiteral("hello required"));
        socket->disconnectFromHost();
        return;
    }

    if (method == QStringLiteral("ping")) {
        QJsonObject result;
        result.insert(QStringLiteral("pong"), true);
        sendResult(socket, id, result);
        return;
    }
    if (method == QStringLiteral("windows")) {
        QJsonObject result = windows(params);
        sendResult(socket, id, result);
        return;
    }
    if (method == QStringLiteral("query")) {
        QJsonObject result = query(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("act")) {
        QJsonObject result = act(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("screenshot")) {
        QByteArray binary;
        QJsonObject result = screenshot(params, &binary);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendBinaryResult(socket, id, result, binary);
        }
        return;
    }
    if (method == QStringLiteral("get")) {
        QJsonObject result = get(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("set")) {
        QJsonObject result = setProperty(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("call")) {
        QJsonObject result = call(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("items")) {
        QJsonObject result = items(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("source")) {
        QJsonObject result = source(params);
        if (result.contains(QStringLiteral("error"))) {
            sendError(socket, id, result.value(QStringLiteral("error")).toString(),
                      result.value(QStringLiteral("message")).toString());
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("shutdown")) {
        sendResult(socket, id, QJsonObject{});
        socket->disconnectFromHost();
        return;
    }
    sendError(socket, id, QStringLiteral("E_PROTOCOL"),
              QStringLiteral("unknown method: %1").arg(method));
}

QJsonObject Server::windows(const QJsonObject& params)
{
    const bool includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    QList<QWidget*> roots = QApplication::topLevelWidgets();
    std::sort(roots.begin(), roots.end(), [](QWidget* left, QWidget* right) {
        return left->winId() < right->winId();
    });

    QJsonArray list;
    for (QWidget* root : roots) {
        if (!root || !root->isWindow())
            continue;
        if (!includeHidden && !root->isVisible())
            continue;
        const QPoint global = root->mapToGlobal(QPoint(0, 0));
        QJsonObject screenPos;
        screenPos.insert(QStringLiteral("x"), global.x());
        screenPos.insert(QStringLiteral("y"), global.y());
        QJsonObject size;
        size.insert(QStringLiteral("w"), root->width());
        size.insert(QStringLiteral("h"), root->height());
        QJsonObject window;
        window.insert(QStringLiteral("id"), idFor(root));
        window.insert(QStringLiteral("class"),
                      QString::fromLatin1(root->metaObject()->className()));
        window.insert(QStringLiteral("objectName"), root->objectName());
        window.insert(QStringLiteral("title"), root->windowTitle());
        window.insert(QStringLiteral("rect"), size);
        window.insert(QStringLiteral("screen_pos"), screenPos);
        window.insert(QStringLiteral("visible"), root->isVisible());
        window.insert(QStringLiteral("minimized"), root->isMinimized());
        window.insert(QStringLiteral("active"),
                      QApplication::activeWindow() == root);
        window.insert(QStringLiteral("modal"),
                      root->isModal() ||
                          QApplication::activeModalWidget() == root);
        const Qt::WindowFlags flags = root->windowFlags();
        window.insert(QStringLiteral("popup"), flags.testFlag(Qt::Popup));
        window.insert(QStringLiteral("tooltip"), flags.testFlag(Qt::ToolTip));
        if (QScreen* screen = root->screen())
            window.insert(QStringLiteral("screen"), screen->name());
        window.insert(
            QStringLiteral("child_count"),
            root->findChildren<QWidget*>(QString(),
                                         Qt::FindDirectChildrenOnly)
                .size());
        list.append(window);
    }

    QJsonObject result;
    result.insert(QStringLiteral("windows"), list);
    result.insert(QStringLiteral("count"), list.size());
    return result;
}

QJsonObject Server::query(const QJsonObject& params)
{
    QueryContext context;
    context.where = params.value(QStringLiteral("where")).toObject();
    context.hasWhere = !context.where.isEmpty();
    static const QSet<QString> allowedWhere{
        QStringLiteral("id"), QStringLiteral("class"),
        QStringLiteral("object_name"), QStringLiteral("objectName"),
        QStringLiteral("qml_id"), QStringLiteral("qml_type"),
        QStringLiteral("text"), QStringLiteral("props")};
    for (auto it = context.where.begin(); it != context.where.end(); ++it) {
        if (!allowedWhere.contains(it.key())) {
            QJsonObject error;
            error.insert(QStringLiteral("error"),
                         QStringLiteral("E_INVALID_FILTER"));
            error.insert(
                QStringLiteral("message"),
                QStringLiteral(
                    "unsupported locator key: %1 (supported: id, class, "
                    "object_name/objectName, qml_id, qml_type, text, props)")
                    .arg(it.key()));
            return error;
        }
        if (it.key() == QStringLiteral("props")) {
            if (!it.value().isObject()) {
                QJsonObject error;
                error.insert(QStringLiteral("error"),
                             QStringLiteral("E_INVALID_FILTER"));
                error.insert(QStringLiteral("message"),
                             QStringLiteral("props filter must be an object"));
                return error;
            }
        } else if (!validMatcher(it.value())) {
            QJsonObject error;
            error.insert(QStringLiteral("error"),
                         QStringLiteral("E_INVALID_FILTER"));
            error.insert(
                QStringLiteral("message"),
                QStringLiteral("invalid matcher for %1 (use string, wildcard, "
                               "or contains/startsWith/regex/equals)")
                    .arg(it.key()));
            return error;
        }
    }
    context.props = params.value(QStringLiteral("props"));
    context.includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    context.maxDepth = params.value(QStringLiteral("max_depth")).toInt(-1);
    const int limit = params.value(QStringLiteral("limit")).toInt(200);
    const bool countOnly =
        params.value(QStringLiteral("count_only")).toBool(false);

    const QString coords =
        params.value(QStringLiteral("coords")).toString(QStringLiteral("parent"));
    if (coords != QStringLiteral("parent") &&
        coords != QStringLiteral("screen")) {
        QJsonObject error;
        error.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("coords must be parent or screen"));
        return error;
    }
    context.screenCoords = coords == QStringLiteral("screen");
    QWidget* scopeRoot = nullptr;
    if (params.contains(QStringLiteral("scope"))) {
        const QJsonValue scope = params.value(QStringLiteral("scope"));
        if (!scope.isNull() && !scope.isUndefined()) {
            QString scopeError;
            scopeRoot = resolveTarget(scope, &scopeError, -1,
                                      context.includeHidden);
            if (!scopeRoot) {
                QJsonObject error;
                error.insert(QStringLiteral("error"), resolveErrorCode());
                error.insert(QStringLiteral("message"), scopeError);
                return error;
            }
        }
    }
    if (params.value(QStringLiteral("occlusion")).toBool(false)) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_NOT_IMPLEMENTED"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("occlusion is not implemented by the linked engine"));
        return error;
    }
    if (limit < 1) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_INVALID_ARGUMENT"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("limit must be >= 1"));
        return error;
    }

    QList<QWidget*> roots;
    if (scopeRoot) {
        roots.append(scopeRoot);
    } else {
        const QList<QWidget*> topLevels = QApplication::topLevelWidgets();
        QSet<QWidget*> topLevelSet;
        for (QWidget* root : topLevels)
            topLevelSet.insert(root);
        for (QWidget* root : topLevels) {
            bool nested = false;
            for (QWidget* parent = root->parentWidget(); parent;
                 parent = parent->parentWidget()) {
                if (topLevelSet.contains(parent)) {
                    nested = true;
                    break;
                }
            }
            if (!nested)
                roots.append(root);
        }
    }
    if (countOnly) {
        for (QWidget* root : roots) {
            if (isCandidate(root, context.includeHidden))
                countNode(root, context, 0);
        }
        QJsonObject result;
        result.insert(QStringLiteral("count"),
                      context.hasWhere ? context.matched : context.total);
        return result;
    }

    QJsonArray tree;
    for (QWidget* root : roots) {
        if (!isCandidate(root, context.includeHidden))
            continue;
        bool any = false;
        const QJsonObject node = buildNode(root, context, 0, true, &any);
        if (any)
            tree.append(node);
    }

    if (context.tooLarge) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_RESULT_TOO_LARGE"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("result exceeds the %1-node hard cap")
                         .arg(QueryContext::kMaxEmitted));
        return error;
    }

    const int count = context.hasWhere ? context.matched : context.total;
    if (limit > 0 && count > limit) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_RESULT_TOO_LARGE"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("result has %1 nodes (limit %2)")
                         .arg(count)
                         .arg(limit));
        return error;
    }

    QJsonObject result;
    result.insert(QStringLiteral("tree"), tree);
    result.insert(QStringLiteral("count"), count);
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
        2 * 1024 * 1024) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_RESULT_TOO_LARGE"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("result exceeds the 2 MiB payload cap"));
        return error;
    }
    return result;
}

QJsonObject Server::act(const QJsonObject& params)
{
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    QString virtualId;
    if (targetValue.isString()) {
        virtualId = targetValue.toString();
    } else if (targetValue.isObject()) {
        virtualId =
            targetValue.toObject().value(QStringLiteral("id")).toString();
    }
    if (!virtualId.isEmpty() && virtuals_.contains(virtualId))
        return actVirtual(params, virtualId);

    QString error;
    const int nth = params.value(QStringLiteral("nth")).toInt(-1);
    QWidget* widget =
        resolveTarget(params.value(QStringLiteral("target")), &error, nth);
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }
    if (QWidget* top = widget->window(); top && top->isMinimized()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("target window is minimized"));
        return result;
    }

    const QString action = params.value(QStringLiteral("action")).toString();
    const QString button =
        params.value(QStringLiteral("button")).toString(QStringLiteral("left"));
    if (button != QStringLiteral("left")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("only left-button actions are supported"));
        return result;
    }
    if (!params.value(QStringLiteral("modifiers")).toArray().isEmpty()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("modifiers are not supported yet"));
        return result;
    }
    const QJsonValue expect = params.value(QStringLiteral("expect"));
    if (!expect.isNull() && !expect.isUndefined()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("expect is not implemented by the linked engine yet"));
        result.insert(QStringLiteral("dispatched"), false);
        return result;
    }

    QString typeTextAfter;
    QJsonObject resolved;
    resolved.insert(QStringLiteral("id"), idFor(widget));
    resolved.insert(QStringLiteral("class"),
                    QString::fromLatin1(widget->metaObject()->className()));
    resolved.insert(QStringLiteral("objectName"), widget->objectName());

    auto sendClick = [&](QEvent::Type type) {
        const QPointer<QWidget> guard(widget);
        if (guard.isNull())
            return;
        QWidget* current = guard.data();
        QPoint local(current->width() / 2, current->height() / 2);
        const QJsonObject position =
            params.value(QStringLiteral("position")).toObject();
        if (!position.isEmpty()) {
            local = QPoint(position.value(QStringLiteral("x")).toInt(),
                           position.value(QStringLiteral("y")).toInt());
        }
        QWidget* target = current;
        if (!qobject_cast<QComboBox*>(current)) {
            if (QWidget* child = current->childAt(local)) {
                target = child;
                local = child->mapFrom(current, local);
            }
        }
        const QPoint global = target->mapToGlobal(local);
        QMouseEvent event(type, QPointF(local), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };

    if (action == QStringLiteral("click") ||
        action == QStringLiteral("dblclick")) {
        if (action == QStringLiteral("click")) {
            if (auto* combo = qobject_cast<QComboBox*>(widget)) {
                combo->showPopup();
            } else {
                sendClick(QEvent::MouseButtonPress);
                sendClick(QEvent::MouseButtonRelease);
            }
        } else {
            sendClick(QEvent::MouseButtonPress);
            sendClick(QEvent::MouseButtonRelease);
            sendClick(QEvent::MouseButtonDblClick);
            sendClick(QEvent::MouseButtonRelease);
        }
    } else if (action == QStringLiteral("type")) {
        const QPointer<QWidget> guard(widget);
        if (guard.isNull()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
            result.insert(QStringLiteral("message"), QStringLiteral("target was destroyed"));
            return result;
        }
        QWidget* current = guard.data();
        auto isEditable = [](QWidget* candidate) {
            return qobject_cast<QLineEdit*>(candidate) ||
                   qobject_cast<QTextEdit*>(candidate) ||
                   qobject_cast<QPlainTextEdit*>(candidate);
        };
        if (!isEditable(current)) {
            for (QWidget* child :
                 current->findChildren<QWidget*>(QString(),
                                                 Qt::FindChildrenRecursively)) {
                if (child && child->isVisible() && isEditable(child)) {
                    current = child;
                    break;
                }
            }
        }
        if (!isEditable(current)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_NOT_EDITABLE"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("target has no editable child"));
            return result;
        }
        current->setFocus(Qt::OtherFocusReason);
        if (params.value(QStringLiteral("clear")).toBool(false))
            QMetaObject::invokeMethod(current, "clear", Qt::DirectConnection);
        const QString text = params.value(QStringLiteral("text")).toString();
        for (const QChar ch : text) {
            if (guard.isNull())
                break;
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
            QApplication::sendEvent(current, &press);
            QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(ch));
            QApplication::sendEvent(current, &release);
        }
        if (params.value(QStringLiteral("submit")).toBool(false) && !guard.isNull()) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(current, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(current, &release);
        }
        if (!guard.isNull()) {
            if (auto* line = qobject_cast<QLineEdit*>(current))
                typeTextAfter = line->text();
            else if (auto* textEdit = qobject_cast<QTextEdit*>(current))
                typeTextAfter = textEdit->toPlainText();
            else if (auto* plain = qobject_cast<QPlainTextEdit*>(current))
                typeTextAfter = plain->toPlainText();
            resolved.insert(QStringLiteral("id"), idFor(current));
            resolved.insert(QStringLiteral("class"),
                            QString::fromLatin1(current->metaObject()->className()));
            resolved.insert(QStringLiteral("objectName"), current->objectName());
        }
    } else if (action == QStringLiteral("focus")) {
        const QPointer<QWidget> guard(widget);
        if (!guard.isNull())
            guard->setFocus(Qt::OtherFocusReason);
    } else if (action == QStringLiteral("drag")) {
        const QPoint start(widget->width() / 2, widget->height() / 2);
        QPoint end = start;
        const QJsonValue to = params.value(QStringLiteral("to"));
        if (to.isObject() && to.toObject().contains(QStringLiteral("x")) &&
            to.toObject().contains(QStringLiteral("y"))) {
            end = QPoint(to.toObject().value(QStringLiteral("x")).toInt(),
                         to.toObject().value(QStringLiteral("y")).toInt());
        } else if (!to.isNull() && !to.isUndefined()) {
            QString error;
            QWidget* destination = resolveTarget(to, &error);
            if (!destination) {
                QJsonObject result;
                result.insert(QStringLiteral("error"), resolveErrorCode());
                result.insert(QStringLiteral("message"), error);
                return result;
            }
            end = widget->mapFromGlobal(
                destination->mapToGlobal(destination->rect().center()));
        }
        const QPoint startGlobal = widget->mapToGlobal(start);
        const int steps = 5;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                          QPointF(startGlobal), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        for (int step = 1; step <= steps; ++step) {
            const QPoint current = start + (end - start) * step / steps;
            QMouseEvent move(QEvent::MouseMove, QPointF(current),
                             QPointF(widget->mapToGlobal(current)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(widget, &move);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(end),
                            QPointF(widget->mapToGlobal(end)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
    } else if (action == QStringLiteral("key")) {
        const QJsonArray keys = params.value(QStringLiteral("keys")).toArray();
        if (keys.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_INVALID_ARGUMENT"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("key requires keys"));
            return result;
        }
        widget->setFocus(Qt::OtherFocusReason);
        for (const QJsonValue& value : keys) {
            const QKeySequence sequence(value.toString());
            if (sequence.isEmpty())
                continue;
            const int combo = sequence[0];
            const int key = combo & ~int(Qt::KeyboardModifierMask);
            const Qt::KeyboardModifiers modifiers =
                Qt::KeyboardModifiers(combo & int(Qt::KeyboardModifierMask));
            QString text;
            if (key >= 0x20 && key < 0x7f)
                text = QString(QChar(key));
            if (modifiers & Qt::ShiftModifier)
                text = text.toUpper();
            if (modifiers & (Qt::ControlModifier | Qt::AltModifier |
                             Qt::MetaModifier))
                text.clear();
            QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
            QApplication::sendEvent(widget, &press);
            QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
            QApplication::sendEvent(widget, &release);
        }
    } else if (action == QStringLiteral("scroll")) {
        const QPoint angle(int(params.value(QStringLiteral("dx")).toDouble()),
                           int(params.value(QStringLiteral("dy")).toDouble()));
        const QPoint local(widget->width() / 2, widget->height() / 2);
        const QPoint global = widget->mapToGlobal(local);
        QWheelEvent event(QPointF(local), QPointF(global), QPoint(), angle,
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(widget, &event);
    } else if (action == QStringLiteral("hover")) {
        const QPoint local(widget->width() / 2, widget->height() / 2);
        QHoverEvent enter(QEvent::HoverEnter, QPointF(local),
                          QPointF(local));
        QApplication::sendEvent(widget, &enter);
        QHoverEvent move(QEvent::HoverMove, QPointF(local), QPointF(local));
        QApplication::sendEvent(widget, &move);
    } else {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("unsupported action: %1").arg(action));
        return result;
    }

    const int settleMs = params.value(QStringLiteral("settle_ms")).toInt(0);
    if (settleMs > 0)
        QThread::msleep(static_cast<unsigned long>(settleMs));

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("action"), action);
    result.insert(QStringLiteral("dispatched"), true);
    result.insert(QStringLiteral("settled_ms"), settleMs);
    if (action == QStringLiteral("type"))
        result.insert(QStringLiteral("text_after"), typeTextAfter);
    result.insert(QStringLiteral("resolved"), resolved);
    const QPointer<QWidget> observedGuard(widget);
    if (params.value(QStringLiteral("observe")).toBool(true) &&
        !observedGuard.isNull()) {
        QWidget* observedWidget = observedGuard.data();
        QJsonObject observed;
        observed.insert(QStringLiteral("id"), idFor(observedWidget));
        observed.insert(QStringLiteral("class"),
                        QString::fromLatin1(observedWidget->metaObject()->className()));
        observed.insert(QStringLiteral("objectName"), observedWidget->objectName());
        result.insert(QStringLiteral("observed"), observed);
    }
    return result;
}

QJsonObject Server::screenshot(const QJsonObject& params, QByteArray* binary)
{
    const QString annotate =
        params.value(QStringLiteral("annotate")).toString(QStringLiteral("none"));
    if (annotate != QStringLiteral("none") &&
        annotate != QStringLiteral("interactive") &&
        annotate != QStringLiteral("matched") &&
        annotate != QStringLiteral("all") &&
        annotate != QStringLiteral("layers")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("invalid annotate mode: %1").arg(annotate));
        return result;
    }
    const int maxLabels = params.value(QStringLiteral("max_labels")).toInt(50);
    if (maxLabels < 1 || maxLabels > 500) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("max_labels must be between 1 and 500"));
        return result;
    }
    const QString format =
        params.value(QStringLiteral("format")).toString(QStringLiteral("png"));
    if (format != QStringLiteral("png")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("only png screenshots are supported"));
        return result;
    }

    const int nth = params.value(QStringLiteral("nth")).toInt(-1);
    const bool explicitTarget = params.contains(QStringLiteral("target"));
    QWidget* widget = nullptr;
    QRect virtualCrop;
    bool virtualTarget = false;
    QString virtualId;
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    if (targetValue.isString() && virtuals_.contains(targetValue.toString())) {
        if (annotate != QStringLiteral("none")) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_NOT_IMPLEMENTED"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("annotate is not supported for virtual items"));
            return result;
        }
        virtualId = targetValue.toString();
        const VirtualRef ref = virtuals_.value(virtualId);
        if (ref.owner.isNull() || !virtualRect(ref, &virtualCrop)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("virtual item is stale"));
            return result;
        }
        widget = ref.owner.data();
        if (ref.kind == QStringLiteral("tab")) {
            if (auto* tabs = qobject_cast<QTabWidget*>(widget))
                widget = tabs->tabBar();
        } else if (ref.kind == QStringLiteral("combo_item") &&
                   !ref.page.isNull()) {
            widget = ref.page.data();
        }
        virtualTarget = true;
    } else if (explicitTarget) {
        QString error;
        widget = resolveTarget(targetValue, &error, nth);
        if (!widget) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), resolveErrorCode());
            result.insert(QStringLiteral("message"), error);
            return result;
        }
    } else {
        QList<QWidget*> visibleWindows;
        const auto roots = QApplication::topLevelWidgets();
        for (QWidget* root : roots) {
            if (!root || !root->isVisible() || !root->isWindow())
                continue;
            const Qt::WindowFlags flags = root->windowFlags();
            if (flags.testFlag(Qt::Popup) || flags.testFlag(Qt::ToolTip))
                continue;
            visibleWindows.append(root);
        }
        if (visibleWindows.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_NOT_FOUND"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("no visible window"));
            return result;
        }
        if (visibleWindows.size() > 1) {
            QStringList names;
            for (QWidget* root : visibleWindows) {
                names.append(QStringLiteral("%1(%2)")
                                 .arg(root->windowTitle())
                                 .arg(static_cast<qlonglong>(root->winId())));
            }
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_AMBIGUOUS"));
            result.insert(
                QStringLiteral("message"),
                QStringLiteral("multiple visible windows (%1); pass target "
                               "or call windows")
                    .arg(names.join(QStringLiteral(", "))));
            return result;
        }
        widget = visibleWindows.first();
    }
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), QStringLiteral("no visible window"));
        return result;
    }
    if (QWidget* top = widget->window(); top && top->isMinimized()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("target window is minimized"));
        return result;
    }

    const int maxWidth = params.value(QStringLiteral("max_width")).toInt(1024);
    if (maxWidth < 0) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"), QStringLiteral("max_width must be >= 0"));
        return result;
    }

    QPixmap pixmap;
    if (virtualTarget) {
        pixmap = widget->grab(virtualCrop);
    } else {
        // Grab the widget, not the native window. grabWindow() includes the
        // native frame/title bar on Windows, which offsets every annotation.
        pixmap = widget->grab();
    }
    if (pixmap.isNull()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("nothing to capture"));
        return result;
    }
    const qreal dpr = pixmap.devicePixelRatio();
    const int originalWidth = pixmap.width();
    const int logicalWidth = qRound(pixmap.width() / dpr);
    if (maxWidth > 0 && logicalWidth > maxWidth) {
        pixmap = pixmap.scaledToWidth(qRound(maxWidth * dpr),
                                      Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(dpr);
    }

    QImage canvas = pixmap.toImage();
    QJsonArray legend;
    int annotateTotal = 0;
    int annotateShown = 0;
    bool annotateTruncated = false;
    if (annotate != QStringLiteral("none")) {
        const QJsonObject where = params.value(QStringLiteral("where")).toObject();
        QList<QWidget*> nodes =
            (annotate == QStringLiteral("matched") && !where.isEmpty())
                ? collectMatches(where)
                : collectMatches(QJsonObject{});
        if (annotate == QStringLiteral("interactive")) {
            QList<QWidget*> interactive;
            for (QWidget* candidate : nodes) {
                if (candidate->focusPolicy() != Qt::NoFocus)
                    interactive.append(candidate);
            }
            nodes = interactive;
        }

        struct Candidate {
            QWidget* node = nullptr;
            QRect rect;
        };
        struct VirtualAnnotation {
            QString id;
            QRect rect;
            QString className;
            QString text;
            QString objectName;
            QString qmlId;
            bool qml = false;
            bool enabled = true;
        };
        QList<VirtualAnnotation> virtualAnnotations;
        auto addVirtualActions = [&](const QList<QAction*>& actions,
                                     const std::function<QRect(QAction*)>& geometry) {
            for (int i = 0; i < actions.size(); ++i) {
                QAction* action = actions.at(i);
                if (!action || !action->isVisible() || action->isSeparator())
                    continue;
                const QRect rect = geometry(action);
                if (rect.isEmpty())
                    continue;
                QJsonObject props;
                props.insert(QStringLiteral("text"), action->text());
                props.insert(QStringLiteral("enabled"), action->isEnabled());
                props.insert(QStringLiteral("checked"), action->isChecked());
                const QString id = registerVirtual(
                    widget, QStringLiteral("menu_item"),
                    QStringLiteral("QAction"), i, rect, props,
                    QPersistentModelIndex(), action);
                virtualAnnotations.append(
                    {id, rect, QStringLiteral("QAction"),
                     action->text(), QString(), QString(), false,
                     action->isEnabled()});
            }
        };
        if (annotate != QStringLiteral("matched")) {
            if (auto* menu = qobject_cast<QMenu*>(widget)) {
                addVirtualActions(menu->actions(), [menu](QAction* action) {
                    return menu->actionGeometry(action);
                });
            } else if (auto* bar = qobject_cast<QMenuBar*>(widget)) {
                addVirtualActions(bar->actions(), [bar](QAction* action) {
                    return bar->actionGeometry(action);
                });
            } else if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
                if (QQuickItem* root = quick->rootObject()) {
                    int serial = 0;
                    std::function<void(QQuickItem*)> addQml =
                        [&](QQuickItem* item) {
                            if (!item || !item->isVisible())
                                return;
                            const QRect rect = qmlItemRect(item, quick);
                            if (!rect.isEmpty()) {
                                QJsonObject props;
                                props.insert(QStringLiteral("text"),
                                             item->property("text").toString());
                                props.insert(QStringLiteral("qml_type"),
                                             qmlTypeName(item));
                                const QString id = registerQml(
                                    widget, item, serial++, rect, props);
                                virtualAnnotations.append(
                                    {id, rect, qmlTypeName(item),
                                     item->property("text").toString(),
                                     item->objectName(), qmlIdName(item),
                                     true, true});
                            }
                            for (QQuickItem* child : item->childItems())
                                addQml(child);
                        };
                    addQml(root);
                }
            }
        }
        QList<Candidate> candidates;
        for (QWidget* node : nodes) {
            if (node == widget || !widget->isAncestorOf(node))
                continue;
            const QRect rect(node->mapTo(widget, QPoint(0, 0)), node->size());
            if (rect.intersects(widget->rect()))
                candidates.append({node, rect});
            if (candidates.size() >= 200)
                break;
        }

        // In "all" mode keep leaf controls: containers only add nested boxes
        // and labels without adding actionable information.
        if (annotate == QStringLiteral("all")) {
            QList<Candidate> leaves;
            for (int i = 0; i < candidates.size(); ++i) {
                bool containsChild = false;
                for (int j = 0; j < candidates.size(); ++j) {
                    if (i == j)
                        continue;
                    if (candidates[i].rect.contains(candidates[j].rect) &&
                        candidates[i].rect != candidates[j].rect) {
                        containsChild = true;
                        break;
                    }
                }
                if (!containsChild)
                    leaves.append(candidates[i]);
            }
            if (!leaves.isEmpty())
                candidates = leaves;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& left, const Candidate& right) {
                      if (left.rect.top() != right.rect.top())
                          return left.rect.top() < right.rect.top();
                      return left.rect.left() < right.rect.left();
                  });
        annotateTotal = candidates.size() + virtualAnnotations.size();
        annotateTruncated = annotateTotal > maxLabels;
        if (candidates.size() > maxLabels)
            candidates = candidates.mid(0, maxLabels);
        const int remaining = qMax(0, maxLabels - candidates.size());
        if (virtualAnnotations.size() > remaining)
            virtualAnnotations = virtualAnnotations.mid(0, remaining);
        annotateShown = candidates.size() + virtualAnnotations.size();

        const int originalWidth = canvas.width();
        const int logicalImageWidth = qRound(originalWidth / dpr);
        const double logicalScale =
            widget->width() > 0 ? double(logicalImageWidth) / widget->width() : 1.0;
        const double deviceScale =
            widget->width() > 0 ? double(originalWidth) / widget->width() : 1.0;

        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, false);
        QFont font = painter.font();
        font.setPixelSize(qMax(8, qRound(9 * dpr)));
        painter.setFont(font);
        QPen boxPen(QColor(220, 40, 40));
        boxPen.setWidth(qMax(1, qRound(2 * dpr)));
        painter.setPen(boxPen);

        struct BadgeTarget {
            QRect rect;
            int number = 0;
        };
        int number = 0;
        QList<BadgeTarget> badgeTargets;
        for (const Candidate& candidate : candidates) {
            ++number;
            int depth = 0;
            for (QWidget* parent = candidate.node->parentWidget();
                 parent && parent != widget; parent = parent->parentWidget())
                ++depth;
            QPen candidatePen = boxPen;
            if (annotate == QStringLiteral("layers") && depth > 0) {
                candidatePen.setStyle(Qt::DashLine);
                candidatePen.setWidth(qMax(1, qRound(1 * dpr)));
            }
            painter.setPen(candidatePen);
            QRect drawRect(
                qRound(candidate.rect.x() * deviceScale),
                qRound(candidate.rect.y() * deviceScale),
                qRound(candidate.rect.width() * deviceScale),
                qRound(candidate.rect.height() * deviceScale));
            drawRect = drawRect.intersected(
                QRect(0, 0, originalWidth, canvas.height()));
            if (drawRect.isEmpty())
                continue;
            if (annotate == QStringLiteral("layers"))
                drawRect = drawRect.adjusted(
                    qRound(depth * dpr), qRound(depth * dpr),
                    -qRound(depth * dpr), -qRound(depth * dpr));
            painter.drawRect(drawRect);
            badgeTargets.append({drawRect, number});

            QJsonObject rect;
            rect.insert(QStringLiteral("x"), candidate.rect.x());
            rect.insert(QStringLiteral("y"), candidate.rect.y());
            rect.insert(QStringLiteral("w"), candidate.rect.width());
            rect.insert(QStringLiteral("h"), candidate.rect.height());
            QJsonObject imageRect;
            imageRect.insert(QStringLiteral("x"), qRound(candidate.rect.x() * logicalScale));
            imageRect.insert(QStringLiteral("y"), qRound(candidate.rect.y() * logicalScale));
            imageRect.insert(QStringLiteral("w"), qRound(candidate.rect.width() * logicalScale));
            imageRect.insert(QStringLiteral("h"), qRound(candidate.rect.height() * logicalScale));
            QJsonObject entry;
            entry.insert(QStringLiteral("n"), number);
            entry.insert(QStringLiteral("id"), idFor(candidate.node));
            entry.insert(QStringLiteral("objectName"), candidate.node->objectName());
            entry.insert(QStringLiteral("class"),
                         QString::fromLatin1(candidate.node->metaObject()->className()));
            QString text = candidate.node->property("text").toString();
            if (auto* line = qobject_cast<QLineEdit*>(candidate.node)) {
                if (line->echoMode() != QLineEdit::Normal)
                    text = QStringLiteral("***");
            }
            entry.insert(QStringLiteral("text"), text);
            entry.insert(QStringLiteral("enabled"), candidate.node->isEnabled());
            entry.insert(QStringLiteral("focusable"),
                         candidate.node->focusPolicy() != Qt::NoFocus);
            entry.insert(QStringLiteral("depth"), depth);
            entry.insert(QStringLiteral("parent_id"),
                         idFor(candidate.node->parentWidget()));
            entry.insert(QStringLiteral("virtual"), false);
            entry.insert(QStringLiteral("rect"), rect);
            entry.insert(QStringLiteral("image_rect"), imageRect);
            const QPoint screen =
                candidate.node->mapToGlobal(QPoint(0, 0));
            QJsonObject screenRect;
            screenRect.insert(QStringLiteral("x"), screen.x());
            screenRect.insert(QStringLiteral("y"), screen.y());
            screenRect.insert(QStringLiteral("w"), candidate.rect.width());
            screenRect.insert(QStringLiteral("h"), candidate.rect.height());
            entry.insert(QStringLiteral("screen_rect"), screenRect);
            legend.append(entry);
        }

        for (const VirtualAnnotation& annotation : virtualAnnotations) {
            ++number;
            const QRect drawRect =
                QRect(qRound(annotation.rect.x() * deviceScale),
                      qRound(annotation.rect.y() * deviceScale),
                      qRound(annotation.rect.width() * deviceScale),
                      qRound(annotation.rect.height() * deviceScale))
                    .intersected(QRect(0, 0, originalWidth, canvas.height()));
            if (drawRect.isEmpty())
                continue;
            painter.setPen(boxPen);
            painter.drawRect(drawRect);
            badgeTargets.append({drawRect, number});

            QJsonObject rect;
            rect.insert(QStringLiteral("x"), annotation.rect.x());
            rect.insert(QStringLiteral("y"), annotation.rect.y());
            rect.insert(QStringLiteral("w"), annotation.rect.width());
            rect.insert(QStringLiteral("h"), annotation.rect.height());
            QJsonObject imageRect;
            imageRect.insert(QStringLiteral("x"),
                             qRound(annotation.rect.x() * logicalScale));
            imageRect.insert(QStringLiteral("y"),
                             qRound(annotation.rect.y() * logicalScale));
            imageRect.insert(QStringLiteral("w"),
                             qRound(annotation.rect.width() * logicalScale));
            imageRect.insert(QStringLiteral("h"),
                             qRound(annotation.rect.height() * logicalScale));
            QJsonObject entry;
            entry.insert(QStringLiteral("n"), number);
            entry.insert(QStringLiteral("id"), annotation.id);
            entry.insert(QStringLiteral("objectName"), annotation.objectName);
            entry.insert(QStringLiteral("qml_id"), annotation.qmlId);
            entry.insert(QStringLiteral("class"), annotation.className);
            entry.insert(QStringLiteral("text"), annotation.text);
            entry.insert(QStringLiteral("enabled"), annotation.enabled);
            entry.insert(QStringLiteral("focusable"), false);
            entry.insert(QStringLiteral("qml"), annotation.qml);
            entry.insert(QStringLiteral("depth"), 0);
            entry.insert(QStringLiteral("parent_id"), idFor(widget));
            entry.insert(QStringLiteral("virtual"), true);
            entry.insert(QStringLiteral("rect"), rect);
            entry.insert(QStringLiteral("image_rect"), imageRect);
            const QPoint screen =
                widget->mapToGlobal(annotation.rect.topLeft());
            QJsonObject screenRect;
            screenRect.insert(QStringLiteral("x"), screen.x());
            screenRect.insert(QStringLiteral("y"), screen.y());
            screenRect.insert(QStringLiteral("w"), annotation.rect.width());
            screenRect.insert(QStringLiteral("h"), annotation.rect.height());
            entry.insert(QStringLiteral("screen_rect"), screenRect);
            legend.append(entry);
        }
        annotateShown = legend.size();

        // Draw all boxes first, then all badges. Otherwise a later box can
        // paint over an earlier badge in nested/overlapping layouts.
        QList<QRect> badgeRects;
        for (const BadgeTarget& target : badgeTargets) {
            const QRect& drawRect = target.rect;
            const int badgeW =
                qMin(drawRect.width(), qRound(20 * dpr));
            const int badgeH =
                qMin(drawRect.height(), qRound(14 * dpr));
            const QPoint gap(qRound(2 * dpr), qRound(2 * dpr));
            const bool smallTarget =
                drawRect.width() < qRound(40 * dpr) ||
                drawRect.height() < qRound(28 * dpr);
            QList<QPoint> positions;
            if (smallTarget) {
                positions.append(
                    QPoint(drawRect.left(), drawRect.top() - badgeH - gap.y()));
                positions.append(
                    QPoint(drawRect.left(), drawRect.bottom() + gap.y()));
            }
            positions.append(drawRect.topLeft() + gap);
            positions.append(QPoint(drawRect.right() - badgeW - gap.x(),
                                    drawRect.top() + gap.y()));
            positions.append(QPoint(drawRect.left() + gap.x(),
                                    drawRect.bottom() - badgeH - gap.y()));
            positions.append(QPoint(drawRect.right() - badgeW - gap.x(),
                                    drawRect.bottom() - badgeH - gap.y()));
            if (!smallTarget) {
                positions.append(
                    QPoint(drawRect.left(), drawRect.top() - badgeH - gap.y()));
                positions.append(
                    QPoint(drawRect.left(), drawRect.bottom() + gap.y()));
            }
            const QRect canvasRect(0, 0, canvas.width(), canvas.height());
            QRect badge;
            for (int positionIndex = 0;
                 positionIndex < positions.size(); ++positionIndex) {
                const bool outside =
                    smallTarget ? positionIndex < 2
                                : positionIndex >= 4;
                QRect candidateBadge(positions.at(positionIndex),
                                     QSize(badgeW, badgeH));
                candidateBadge = candidateBadge.intersected(
                    outside ? canvasRect : drawRect);
                if (candidateBadge.isEmpty())
                    continue;
                bool overlaps = false;
                for (const QRect& used : badgeRects) {
                    if (candidateBadge.intersects(used)) {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps) {
                    badge = candidateBadge;
                    break;
                }
            }
            if (badge.isEmpty())
                badge = QRect(drawRect.topLeft(), QSize(badgeW, badgeH))
                            .intersected(drawRect);
            if (badge.isEmpty())
                continue;
            badgeRects.append(badge);
            painter.fillRect(badge, QColor(255, 255, 255, 230));
            painter.setPen(QPen(QColor(220, 40, 40), 1));
            painter.drawRect(badge);
            painter.drawText(badge, Qt::AlignCenter,
                             QString::number(target.number));
        }
    }

    QBuffer buffer(binary);
    if (!buffer.open(QIODevice::WriteOnly)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INJECTION_FAILED"));
        result.insert(QStringLiteral("message"), QStringLiteral("PNG buffer failed"));
        return result;
    }
    QImageWriter writer(&buffer, "png");
    writer.setCompression(1);
    if (!writer.write(canvas)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INJECTION_FAILED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("PNG encoding failed: %1")
                          .arg(writer.errorString()));
        return result;
    }

    QJsonObject result;
    if (binary->size() > 64 * 1024 * 1024) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("E_IMAGE_TOO_LARGE"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("encoded image exceeds the frame limit"));
        return error;
    }
    const double scale =
        originalWidth > 0 ? double(pixmap.width()) / originalWidth : 1.0;
    const int width = qRound(canvas.width() / dpr);
    const int height = qRound(canvas.height() / dpr);
    result.insert(QStringLiteral("format"), QStringLiteral("png"));
    result.insert(QStringLiteral("width"), width);
    result.insert(QStringLiteral("height"), height);
    result.insert(QStringLiteral("pixel_width"), canvas.width());
    result.insert(QStringLiteral("pixel_height"), canvas.height());
    result.insert(QStringLiteral("scale"), scale);
    result.insert(QStringLiteral("bytes"), binary->size());
    result.insert(QStringLiteral("dpr"), dpr);
    QJsonObject image;
    image.insert(QStringLiteral("width"), width);
    image.insert(QStringLiteral("height"), height);
    image.insert(QStringLiteral("pixel_width"), canvas.width());
    image.insert(QStringLiteral("pixel_height"), canvas.height());
    image.insert(QStringLiteral("scale"), scale);
    image.insert(QStringLiteral("dpr"), dpr);
    image.insert(QStringLiteral("bytes"), binary->size());
    image.insert(QStringLiteral("format"), QStringLiteral("png"));
    result.insert(QStringLiteral("image"), image);
    result.insert(QStringLiteral("legend"), legend);
    if (QWidget* top = widget->window()) {
        QJsonObject window;
        window.insert(QStringLiteral("id"), idFor(top));
        window.insert(QStringLiteral("title"), top->windowTitle());
        window.insert(QStringLiteral("class"),
                      QString::fromLatin1(top->metaObject()->className()));
        window.insert(QStringLiteral("objectName"), top->objectName());
        window.insert(QStringLiteral("active"),
                      QApplication::activeWindow() == top);
        result.insert(QStringLiteral("window"), window);
    }
    if (annotate != QStringLiteral("none")) {
        result.insert(QStringLiteral("annotate_total"), annotateTotal);
        result.insert(QStringLiteral("annotate_shown"), annotateShown);
        result.insert(QStringLiteral("annotate_truncated"), annotateTruncated);
    }
    if (virtualTarget) {
        result.insert(QStringLiteral("virtual"), true);
        result.insert(QStringLiteral("target"), virtualId);
    }
    return result;
}

QJsonObject Server::get(const QJsonObject& params)
{
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    if (targetValue.isString() && virtuals_.contains(targetValue.toString()))
        return virtualItem(targetValue.toString());

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        params.value(QStringLiteral("nth")).toInt(-1),
        params.value(QStringLiteral("include_hidden")).toBool(false));
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }
    return nodeFor(widget, params.value(QStringLiteral("props")));
}

QJsonObject Server::setProperty(const QJsonObject& params)
{
    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        params.value(QStringLiteral("nth")).toInt(-1),
        params.value(QStringLiteral("include_hidden")).toBool(false));
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }
    const QString name = params.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"), QStringLiteral("name is required"));
        return result;
    }
    const QMetaObject* meta = widget->metaObject();
    const int index = meta->indexOfProperty(name.toUtf8().constData());
    if (index < 0) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("no such property: %1").arg(name));
        return result;
    }
    const QMetaProperty property = meta->property(index);
    if (!property.isWritable()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_DISABLED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("property is not writable: %1").arg(name));
        return result;
    }
    const QVariant oldValue = property.read(widget);
    const QVariant newValue =
        params.value(QStringLiteral("value")).toVariant();
    if (!widget->setProperty(name.toUtf8().constData(), newValue)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("setProperty failed: %1").arg(name));
        return result;
    }

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("old"), QJsonValue::fromVariant(oldValue));
    result.insert(QStringLiteral("new"),
                  QJsonValue::fromVariant(property.read(widget)));
    return result;
}

QJsonObject Server::call(const QJsonObject& params)
{
    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        params.value(QStringLiteral("nth")).toInt(-1),
        params.value(QStringLiteral("include_hidden")).toBool(false));
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }
    const QString method = params.value(QStringLiteral("method")).toString();
    if (method.isEmpty()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"), QStringLiteral("method is required"));
        return result;
    }

    const QJsonArray args = params.value(QStringLiteral("args")).toArray();
    if (args.size() > 4) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("at most 4 arguments are supported"));
        return result;
    }

    QMetaMethod chosen;
    for (int i = 0; i < widget->metaObject()->methodCount(); ++i) {
        const QMetaMethod candidate = widget->metaObject()->method(i);
        if (candidate.methodType() == QMetaMethod::Signal)
            continue;
        if (QString::fromLatin1(candidate.name()) != method)
            continue;
        if (candidate.parameterCount() != args.size())
            continue;
        chosen = candidate;
        break;
    }
    if (!chosen.isValid()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("no matching method: %1/%2")
                          .arg(method)
                          .arg(args.size()));
        return result;
    }

    QVector<QVariant> values;
    QVector<QByteArray> typeNames;
    values.reserve(args.size());
    typeNames.reserve(args.size());
    const QList<QByteArray> parameterTypes = chosen.parameterTypes();
    for (int i = 0; i < args.size(); ++i) {
        QVariant value = args.at(i).toVariant();
        const int typeId = chosen.parameterType(i);
        if (value.userType() != typeId && !value.convert(typeId)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_INVALID_ARGUMENT"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("cannot convert argument %1 to %2")
                              .arg(i)
                              .arg(QString::fromLatin1(
                                  QMetaType::typeName(typeId))));
            return result;
        }
        values.append(value);
        typeNames.append(parameterTypes.at(i));
    }

    bool invoked = false;
    if (args.isEmpty()) {
        invoked = chosen.invoke(widget, Qt::DirectConnection);
    } else {
        QGenericArgument arguments[4];
        for (int i = 0; i < values.size(); ++i)
            arguments[i] = QGenericArgument(typeNames.at(i).constData(),
                                            values.at(i).data());
        invoked = chosen.invoke(widget, Qt::DirectConnection, arguments[0],
                                arguments[1], arguments[2], arguments[3]);
    }
    if (!invoked) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("method not invokable: %1").arg(method));
        return result;
    }

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("method"), method);
    return result;
}

QJsonObject Server::items(const QJsonObject& params)
{
    QString error;
    const QJsonValue ownerTarget =
        params.contains(QStringLiteral("owner"))
            ? params.value(QStringLiteral("owner"))
            : params.value(QStringLiteral("target"));
    QWidget* owner = resolveTarget(
        ownerTarget, &error,
        params.value(QStringLiteral("nth")).toInt(-1),
        params.value(QStringLiteral("include_hidden")).toBool(false));
    if (!owner) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }
    const QJsonObject where = params.value(QStringLiteral("where")).toObject();
    const bool includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    const bool compact = params.value(QStringLiteral("compact")).toBool(false);
    const int limit = params.value(QStringLiteral("limit")).toInt(50);
    const int offset = params.value(QStringLiteral("offset")).toInt(0);
    if (limit < 1 || offset < 0) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("limit must be >= 1 and offset >= 0"));
        return result;
    }

    const QString ownerId = idFor(owner);
    int total = 0;
    QVector<QJsonObject> matched;

    auto makeItem = [&](const QString& kind, const QString& cls, int index,
                        const QRect& rect, const QJsonObject& props,
                        const QPersistentModelIndex& persistent,
                        QAction* action = nullptr, QWidget* page = nullptr,
                        int logicalIndex = -1) {
        const QString virtualId = registerVirtual(
            owner, kind, cls, index, rect, props, persistent, action, page,
            logicalIndex);
        QJsonObject item;
        item.insert(QStringLiteral("id"), virtualId);
        item.insert(QStringLiteral("kind"), kind);
        if (!compact) {
            item.insert(QStringLiteral("virtual"), true);
            item.insert(QStringLiteral("class"), cls);
            item.insert(QStringLiteral("parent"), ownerId);
        }
        QJsonObject r;
        r.insert(QStringLiteral("x"), rect.x());
        r.insert(QStringLiteral("y"), rect.y());
        r.insert(QStringLiteral("w"), rect.width());
        r.insert(QStringLiteral("h"), rect.height());
        item.insert(QStringLiteral("rect"), r);
        QJsonObject itemProps = props;
        if (compact && kind == QStringLiteral("cell"))
            itemProps.remove(QStringLiteral("selected"));
        item.insert(QStringLiteral("props"), itemProps);
        if (!compact) {
            item.insert(QStringLiteral("child_count"), 0);
            item.insert(QStringLiteral("children"), QJsonArray{});
            item.insert(QStringLiteral("source"), QStringLiteral("adapter"));
        }
        return item;
    };

    auto consider = [&](const QString& kind, const QString& cls, int index,
                        const QRect& rect, const QJsonObject& props,
                        const QPersistentModelIndex& persistent =
                            QPersistentModelIndex(),
                        QAction* action = nullptr, QWidget* page = nullptr,
                        int logicalIndex = -1) {
        if (itemMatches(props, kind, cls, where))
            matched.append(makeItem(kind, cls, index, rect, props, persistent,
                                    action, page, logicalIndex));
    };

    auto addTabs = [&](QTabBar* bar, QTabWidget* tabWidget) {
        total = bar->count();
        for (int i = 0; i < bar->count(); ++i) {
            if (!includeHidden && !bar->isTabVisible(i))
                continue;
            const QRect rect = bar->tabRect(i);
            if (rect.isEmpty())
                continue;
            QJsonObject props;
            props.insert(QStringLiteral("text"), bar->tabText(i));
            props.insert(QStringLiteral("enabled"), bar->isTabEnabled(i));
            props.insert(QStringLiteral("selected"), bar->currentIndex() == i);
            QWidget* page = tabWidget ? tabWidget->widget(i) : nullptr;
            consider(QStringLiteral("tab"), QStringLiteral("QTabBar::tab"),
                     i, rect, props, QPersistentModelIndex(), nullptr, page,
                     -1);
        }
    };

    if (auto* tabWidget = qobject_cast<QTabWidget*>(owner)) {
        addTabs(tabWidget->tabBar(), tabWidget);
    } else if (auto* bar = qobject_cast<QTabBar*>(owner)) {
        addTabs(bar, nullptr);
    } else if (auto* menuBar = qobject_cast<QMenuBar*>(owner)) {
        const auto actions = menuBar->actions();
        total = 0;
        for (QAction* action : actions)
            if (action && !action->isSeparator())
                ++total;
        for (int i = 0; i < actions.size(); ++i) {
            QAction* action = actions.at(i);
            if (!includeHidden && !action->isVisible())
                continue;
            if (action->isSeparator())
                continue;
            const QRect rect = menuBar->actionGeometry(action);
            if (rect.isEmpty())
                continue;
            QJsonObject props;
            props.insert(QStringLiteral("text"), action->text());
            props.insert(QStringLiteral("enabled"), action->isEnabled());
            props.insert(QStringLiteral("checked"), action->isChecked());
            consider(QStringLiteral("menu_item"), QStringLiteral("QAction"),
                     i, rect, props, QPersistentModelIndex(), action);
        }
    } else if (auto* menu = qobject_cast<QMenu*>(owner)) {
        const auto actions = menu->actions();
        total = 0;
        for (QAction* action : actions)
            if (action && !action->isSeparator())
                ++total;
        for (int i = 0; i < actions.size(); ++i) {
            QAction* action = actions.at(i);
            if (!includeHidden && !action->isVisible())
                continue;
            if (action->isSeparator())
                continue;
            const QRect rect = menu->actionGeometry(action);
            if (rect.isEmpty())
                continue;
            QJsonObject props;
            props.insert(QStringLiteral("text"), action->text());
            props.insert(QStringLiteral("enabled"), action->isEnabled());
            props.insert(QStringLiteral("checked"), action->isChecked());
            consider(QStringLiteral("menu_item"), QStringLiteral("QAction"),
                     i, rect, props, QPersistentModelIndex(), action);
        }
    } else if (auto* header = qobject_cast<QHeaderView*>(owner)) {
        total = header->count();
        const bool horizontal = header->orientation() == Qt::Horizontal;
        for (int i = 0; i < header->count(); ++i) {
            if (!includeHidden && header->isSectionHidden(i))
                continue;
            const int position = header->sectionViewportPosition(i);
            const int size = header->sectionSize(i);
            if (size <= 0)
                continue;
            const QRect local = horizontal
                                    ? QRect(position, 0, size,
                                            header->viewport()->height())
                                    : QRect(0, position,
                                            header->viewport()->width(), size);
            const QRect rect = local.translated(header->viewport()->pos());
            QJsonObject props;
            props.insert(QStringLiteral("text"),
                         header->model()
                             ? header->model()
                                   ->headerData(i, header->orientation(),
                                                Qt::DisplayRole)
                                   .toString()
                             : QString());
            consider(QStringLiteral("header_section"),
                     QStringLiteral("QHeaderView::section"), i, rect, props,
                     QPersistentModelIndex(), nullptr, nullptr, i);
        }
    } else if (auto* view = qobject_cast<QTableView*>(owner)) {
        auto* model = view->model();
        if (!model) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("view has no model"));
            return result;
        }
        total = model->rowCount() * model->columnCount();
        const QRect viewportRect = view->viewport()->rect();
        int firstRow = view->rowAt(viewportRect.top());
        int lastRow = view->rowAt(viewportRect.bottom());
        int firstColumn = view->columnAt(viewportRect.left());
        int lastColumn = view->columnAt(viewportRect.right());
        if (firstRow < 0)
            firstRow = 0;
        if (lastRow < 0)
            lastRow = model->rowCount() - 1;
        if (firstColumn < 0)
            firstColumn = 0;
        if (lastColumn < 0)
            lastColumn = model->columnCount() - 1;
        firstRow = qMax(0, firstRow);
        firstColumn = qMax(0, firstColumn);
        lastRow = qMin(model->rowCount() - 1, lastRow);
        lastColumn = qMin(model->columnCount() - 1, lastColumn);
        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const QModelIndex index = model->index(row, column);
                const QRect local = view->visualRect(index);
                if (local.isEmpty() || !viewportRect.intersects(local))
                    continue;
                const QRect rect = local.translated(view->viewport()->pos());
                QJsonObject props;
                props.insert(QStringLiteral("text"),
                             model->data(index, Qt::DisplayRole).toString());
                props.insert(QStringLiteral("row"), row);
                props.insert(QStringLiteral("column"), column);
                props.insert(QStringLiteral("selected"),
                             view->selectionModel() &&
                                 view->selectionModel()->isSelected(index));
                consider(QStringLiteral("cell"),
                         QStringLiteral("QTableView::cell"),
                         row * model->columnCount() + column, rect, props,
                         QPersistentModelIndex(index));
            }
        }
    } else if (auto* combo = qobject_cast<QComboBox*>(owner)) {
        auto* view = qobject_cast<QAbstractItemView*>(combo->view());
        auto* model = combo->model();
        if (!view || !model) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("combo has no popup view"));
            return result;
        }
        total = model->rowCount();
        if (!view->isVisible()) {
            for (int row = 0; row < model->rowCount(); ++row) {
                QJsonObject props;
                props.insert(QStringLiteral("text"), combo->itemText(row));
                props.insert(QStringLiteral("row"), row);
                props.insert(QStringLiteral("selected"),
                             combo->currentIndex() == row);
                consider(QStringLiteral("combo_item"),
                         QStringLiteral("QComboBox::item"), row,
                         QRect(0, combo->height() + row * 20,
                               combo->width(), 20),
                         props, QPersistentModelIndex(model->index(row, 0)),
                         nullptr, view);
            }
        } else {
            const QRect viewportRect = view->viewport()->rect();
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row, 0);
                const QRect local = view->visualRect(index);
                if (local.isEmpty() || !viewportRect.intersects(local))
                    continue;
                QJsonObject props;
                props.insert(QStringLiteral("text"),
                             model->data(index, Qt::DisplayRole).toString());
                props.insert(QStringLiteral("row"), row);
                props.insert(QStringLiteral("selected"),
                             view->selectionModel() &&
                                 view->selectionModel()->isSelected(index));
                consider(QStringLiteral("combo_item"),
                         QStringLiteral("QComboBox::item"), row,
                         local.translated(view->viewport()->pos()), props,
                         QPersistentModelIndex(index), nullptr, view);
            }
        }
    } else if (auto* tree = qobject_cast<QTreeView*>(owner)) {
        auto* model = tree->model();
        if (!model) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("tree has no model"));
            return result;
        }
        total = model->rowCount() * model->columnCount();
        const QRect viewportRect = tree->viewport()->rect();
        int firstRow = tree->indexAt(viewportRect.topLeft()).row();
        int lastRow = tree->indexAt(viewportRect.bottomRight()).row();
        if (firstRow < 0)
            firstRow = 0;
        if (lastRow < 0)
            lastRow = model->rowCount() - 1;
        for (int row = qMax(0, firstRow);
             row <= qMin(model->rowCount() - 1, lastRow); ++row) {
            for (int column = 0; column < model->columnCount(); ++column) {
                const QModelIndex index = model->index(row, column);
                const QRect local = tree->visualRect(index);
                if (local.isEmpty() || !viewportRect.intersects(local))
                    continue;
                QJsonObject props;
                props.insert(QStringLiteral("text"),
                             model->data(index, Qt::DisplayRole).toString());
                props.insert(QStringLiteral("row"), row);
                props.insert(QStringLiteral("column"), column);
                props.insert(QStringLiteral("selected"),
                             tree->selectionModel() &&
                                 tree->selectionModel()->isSelected(index));
                consider(QStringLiteral("tree_item"),
                         QStringLiteral("QTreeView::item"),
                         row * model->columnCount() + column,
                         local.translated(tree->viewport()->pos()), props,
                         QPersistentModelIndex(index));
            }
        }
    } else if (auto* list = qobject_cast<QListView*>(owner)) {
        auto* model = list->model();
        if (!model) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("list has no model"));
            return result;
        }
        total = model->rowCount();
        const QRect viewportRect = list->viewport()->rect();
        int firstRow = list->indexAt(viewportRect.topLeft()).row();
        int lastRow = list->indexAt(viewportRect.bottomRight()).row();
        if (firstRow < 0)
            firstRow = 0;
        if (lastRow < 0)
            lastRow = model->rowCount() - 1;
        for (int row = qMax(0, firstRow);
             row <= qMin(model->rowCount() - 1, lastRow); ++row) {
            const QModelIndex index = model->index(row, 0);
            const QRect local = list->visualRect(index);
            if (local.isEmpty() || !viewportRect.intersects(local))
                continue;
            QJsonObject props;
            props.insert(QStringLiteral("text"),
                         model->data(index, Qt::DisplayRole).toString());
            props.insert(QStringLiteral("row"), row);
            props.insert(QStringLiteral("selected"),
                         list->selectionModel() &&
                             list->selectionModel()->isSelected(index));
            consider(QStringLiteral("list_item"),
                     QStringLiteral("QListView::item"), row,
                     local.translated(list->viewport()->pos()), props,
                     QPersistentModelIndex(index));
        }
    } else if (auto* view = qobject_cast<QAbstractItemView*>(owner)) {
        auto* model = view->model();
        if (!model) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("abstract item view has no model"));
            return result;
        }
        total = model->rowCount() * qMax(1, model->columnCount());
        const QRect viewportRect = view->viewport()->rect();
        int firstRow = view->indexAt(viewportRect.topLeft()).row();
        int lastRow = view->indexAt(viewportRect.bottomRight()).row();
        if (firstRow < 0)
            firstRow = 0;
        if (lastRow < 0)
            lastRow = model->rowCount() - 1;
        for (int row = qMax(0, firstRow);
             row <= qMin(model->rowCount() - 1, lastRow); ++row) {
            for (int column = 0; column < model->columnCount(); ++column) {
                const QModelIndex index = model->index(row, column);
                const QRect local = view->visualRect(index);
                if (local.isEmpty() || !viewportRect.intersects(local))
                    continue;
                QJsonObject props;
                props.insert(QStringLiteral("text"),
                             model->data(index, Qt::DisplayRole).toString());
                props.insert(QStringLiteral("row"), row);
                props.insert(QStringLiteral("column"), column);
                props.insert(QStringLiteral("selected"),
                             view->selectionModel() &&
                                 view->selectionModel()->isSelected(index));
                consider(QStringLiteral("cell"),
                         QStringLiteral("QAbstractItemView::cell"), row *
                             qMax(1, model->columnCount()) + column,
                         local.translated(viewportRect.topLeft()), props,
                         QPersistentModelIndex(index));
            }
        }
    } else if (auto* bar = qobject_cast<QScrollBar*>(owner)) {
        total = 1;
        QStyleOptionSlider option;
        option.initFrom(bar);
        option.orientation = bar->orientation();
        option.minimum = bar->minimum();
        option.maximum = bar->maximum();
        option.pageStep = bar->pageStep();
        option.singleStep = bar->singleStep();
        option.sliderPosition = bar->sliderPosition();
        option.sliderValue = bar->value();
        const QRect rect = bar->style()->subControlRect(
            QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
        if (!rect.isEmpty()) {
            QJsonObject props;
            props.insert(QStringLiteral("value"), bar->value());
            props.insert(QStringLiteral("minimum"), bar->minimum());
            props.insert(QStringLiteral("maximum"), bar->maximum());
            props.insert(QStringLiteral("page_step"), bar->pageStep());
            consider(QStringLiteral("scrollbar_handle"),
                     QStringLiteral("QScrollBar::handle"), 0, rect, props);
        }
    } else if (auto* slider = qobject_cast<QSlider*>(owner)) {
        total = 1;
        QStyleOptionSlider option;
        option.initFrom(slider);
        option.orientation = slider->orientation();
        option.minimum = slider->minimum();
        option.maximum = slider->maximum();
        option.sliderPosition = slider->sliderPosition();
        option.sliderValue = slider->value();
        option.direction = slider->layoutDirection();
        option.upsideDown =
            slider->orientation() == Qt::Horizontal
                ? slider->invertedAppearance() !=
                      (slider->layoutDirection() == Qt::RightToLeft)
                : !slider->invertedAppearance();
        const QRect rect = slider->style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
        if (rect.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("style has no slider handle"));
            return result;
        }
        QJsonObject props;
        props.insert(QStringLiteral("value"), slider->value());
        props.insert(QStringLiteral("minimum"), slider->minimum());
        props.insert(QStringLiteral("maximum"), slider->maximum());
        consider(QStringLiteral("slider_handle"),
                 QStringLiteral("QSlider::handle"), 0, rect, props);
    } else if (auto* spin = qobject_cast<QSpinBox*>(owner)) {
        total = 2;
        QStyleOptionSpinBox option;
        option.initFrom(spin);
        option.frame = spin->hasFrame();
        option.buttonSymbols = spin->buttonSymbols();
        option.stepEnabled = QAbstractSpinBox::StepUpEnabled |
                             QAbstractSpinBox::StepDownEnabled;
        for (int i = 0; i < 2; ++i) {
            const QStyle::SubControl sub =
                i == 0 ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown;
            const QRect rect = spin->style()->subControlRect(
                QStyle::CC_SpinBox, &option, sub, spin);
            if (rect.isEmpty())
                continue;
            QJsonObject props;
            props.insert(QStringLiteral("value"), spin->value());
            props.insert(QStringLiteral("minimum"), spin->minimum());
            props.insert(QStringLiteral("maximum"), spin->maximum());
            props.insert(QStringLiteral("enabled"), spin->isEnabled());
            consider(QStringLiteral("spin_button"),
                     i == 0 ? QStringLiteral("QSpinBox::up")
                            : QStringLiteral("QSpinBox::down"),
                     i, rect, props);
        }
    } else if (auto* group = qobject_cast<QGroupBox*>(owner)) {
        if (!group->isCheckable()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("group box is not checkable"));
            return result;
        }
        total = 1;
        QStyleOptionGroupBox option;
        option.initFrom(group);
        option.text = group->title();
        option.textAlignment = group->alignment();
        const QRect rect = group->style()->subControlRect(
            QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, group);
        if (rect.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("style has no group box check box"));
            return result;
        }
        QJsonObject props;
        props.insert(QStringLiteral("checked"), group->isChecked());
        props.insert(QStringLiteral("text"), group->title());
        props.insert(QStringLiteral("enabled"), group->isEnabled());
        consider(QStringLiteral("groupbox_check"),
                 QStringLiteral("QGroupBox::check"), 0, rect, props);
    } else if (auto* dock = qobject_cast<QDockWidget*>(owner)) {
        const QList<QAbstractButton*> buttons =
            dock->findChildren<QAbstractButton*>();
        total = buttons.size();
        for (int i = 0; i < buttons.size(); ++i) {
            QAbstractButton* button = buttons.at(i);
            if (!button || !button->isVisible())
                continue;
            const QRect rect(button->mapTo(dock, QPoint(0, 0)),
                             button->size());
            QJsonObject props;
            props.insert(QStringLiteral("text"), button->text());
            props.insert(QStringLiteral("tooltip"), button->toolTip());
            props.insert(QStringLiteral("enabled"), button->isEnabled());
            props.insert(QStringLiteral("floating"), dock->isFloating());
            consider(QStringLiteral("dock_button"),
                     QStringLiteral("QDockWidget::button"), i, rect, props,
                     QPersistentModelIndex(), nullptr, button);
        }
    } else if (auto* toolbar = qobject_cast<QToolBar*>(owner)) {
        const auto actions = toolbar->actions();
        total = actions.size();
        for (int i = 0; i < actions.size(); ++i) {
            QAction* action = actions.at(i);
            if (!action || action->isSeparator() ||
                (!includeHidden && !action->isVisible()))
                continue;
            QWidget* actionWidget = toolbar->widgetForAction(action);
            if (!actionWidget)
                continue;
            if (!includeHidden && !actionWidget->isVisible())
                continue;
            const QRect rect(actionWidget->mapTo(toolbar, QPoint(0, 0)),
                             actionWidget->size());
            QJsonObject props;
            props.insert(QStringLiteral("text"), action->text());
            props.insert(QStringLiteral("enabled"), action->isEnabled());
            props.insert(QStringLiteral("checked"), action->isChecked());
            consider(QStringLiteral("toolbar_action"),
                     QStringLiteral("QToolBar::action"), i, rect, props,
                     QPersistentModelIndex(), action);
        }
    } else if (auto* splitter = qobject_cast<QSplitter*>(owner)) {
        total = qMax(0, splitter->count() - 1);
        for (int i = 0; i < total; ++i) {
            QSplitterHandle* handle = splitter->handle(i);
            if (!handle)
                continue;
            const QRect rect(handle->mapTo(splitter, QPoint(0, 0)),
                             handle->size());
            QJsonObject props;
            props.insert(QStringLiteral("index"), i);
            props.insert(QStringLiteral("orientation"),
                         splitter->orientation() == Qt::Horizontal
                             ? QStringLiteral("horizontal")
                             : QStringLiteral("vertical"));
            consider(QStringLiteral("splitter_handle"),
                     QStringLiteral("QSplitterHandle"), i, rect, props,
                     QPersistentModelIndex(), nullptr, handle);
        }
    } else if (QAccessibleInterface* iface =
                   QAccessible::queryAccessibleInterface(owner)) {
        total = iface->childCount();
        for (int i = 0; i < total; ++i) {
            QAccessibleInterface* child = iface->child(i);
            if (!child)
                continue;
            const QRect screenRect = child->rect();
            if (screenRect.isEmpty())
                continue;
            const QRect rect(owner->mapFromGlobal(screenRect.topLeft()),
                             screenRect.size());
            QJsonObject props;
            props.insert(QStringLiteral("text"),
                         child->text(QAccessible::Name));
            props.insert(QStringLiteral("role"),
                         static_cast<int>(child->role()));
            consider(QStringLiteral("accessible_item"),
                     QStringLiteral("QAccessible::item"), i, rect, props);
        }
        if (total == 0) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("accessible provider found no children"));
            return result;
        }
    } else {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NO_ITEMS"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("no item provider for %1")
                          .arg(QString::fromLatin1(owner->metaObject()->className())));
        return result;
    }

    QJsonArray page;
    const int start = qMin(offset, matched.size());
    const int end = qMin(matched.size(), start + limit);
    for (int i = start; i < end; ++i)
        page.append(matched.at(i));

    QJsonObject result;
    result.insert(QStringLiteral("owner"), ownerId);
    result.insert(QStringLiteral("items"), page);
    result.insert(QStringLiteral("total"), total);
    result.insert(QStringLiteral("visible"), matched.size());
    result.insert(QStringLiteral("truncated"), end < matched.size());
    return result;
}

QString Server::registerVirtual(QWidget* owner, const QString& kind,
                                const QString& className, int index,
                                const QRect& rect, const QJsonObject& props,
                                const QPersistentModelIndex& persistent,
                                QAction* action, QWidget* page,
                                int logicalIndex)
{
    const QString id = tag_ + QStringLiteral(".v") + kind +
                       QStringLiteral(".") + idFor(owner) +
                       QStringLiteral(".") + QString::number(index);
    VirtualRef ref;
    ref.owner = owner;
    ref.kind = kind;
    ref.className = className;
    ref.index = index;
    ref.rect = rect;
    ref.props = props;
    ref.persistent = persistent;
    ref.action = action;
    ref.page = page;
    ref.logicalIndex = logicalIndex;
    virtuals_.insert(id, ref);

    if (!virtualOwners_.contains(owner)) {
        virtualOwners_.insert(owner);
        connect(owner, &QObject::destroyed, this, [this, owner]() {
            for (auto it = virtuals_.begin(); it != virtuals_.end();) {
                if (it->owner.isNull() || it->owner == owner)
                    it = virtuals_.erase(it);
                else
                    ++it;
            }
            virtualOwners_.remove(owner);
        });
    }
    return id;
}

bool Server::virtualRect(const VirtualRef& ref, QRect* rect) const
{
    QWidget* owner = ref.owner.data();
    if (!owner)
        return false;
    if (ref.kind == QStringLiteral("combo_item") && !ref.page.isNull() &&
        ref.persistent.isValid()) {
        if (auto* view = qobject_cast<QAbstractItemView*>(ref.page.data())) {
            const QRect local = view->visualRect(ref.persistent);
            if (!local.isEmpty()) {
                *rect = local.translated(view->viewport()->pos());
                return true;
            }
        }
    }
    if ((ref.kind == QStringLiteral("cell") ||
         ref.kind == QStringLiteral("tree_item") ||
         ref.kind == QStringLiteral("list_item")) &&
        ref.persistent.isValid()) {
        if (auto* view = qobject_cast<QAbstractItemView*>(owner)) {
            const QRect local = view->visualRect(ref.persistent);
            if (!local.isEmpty()) {
                *rect = local.translated(view->viewport()->pos());
                return true;
            }
        }
    }
    if (ref.kind == QStringLiteral("qml_item")) {
        if (ref.qmlObject.isNull())
            return false;
        *rect = ref.rect;
        return !rect->isEmpty();
    }
    if (ref.kind == QStringLiteral("tab")) {
        if (auto* tabs = qobject_cast<QTabWidget*>(owner)) {
            const int index = ref.page ? tabs->indexOf(ref.page) : ref.index;
            if (index >= 0 && index < tabs->count()) {
                *rect = tabs->tabBar()->tabRect(index);
                return !rect->isEmpty();
            }
            return false;
        }
        if (auto* bar = qobject_cast<QTabBar*>(owner)) {
            if (ref.index >= 0 && ref.index < bar->count()) {
                *rect = bar->tabRect(ref.index);
                return !rect->isEmpty();
            }
            return false;
        }
    }
    if (ref.kind == QStringLiteral("menu_item") && !ref.action.isNull()) {
        if (auto* bar = qobject_cast<QMenuBar*>(owner)) {
            *rect = bar->actionGeometry(ref.action);
            return !rect->isEmpty();
        }
        if (auto* menu = qobject_cast<QMenu*>(owner)) {
            *rect = menu->actionGeometry(ref.action);
            return !rect->isEmpty();
        }
    }
    if (ref.kind == QStringLiteral("header_section") &&
        ref.logicalIndex >= 0) {
        if (auto* header = qobject_cast<QHeaderView*>(owner)) {
            const int position =
                header->sectionViewportPosition(ref.logicalIndex);
            const int size = header->sectionSize(ref.logicalIndex);
            if (size > 0) {
                const QPoint origin = header->viewport()->pos();
                if (header->orientation() == Qt::Horizontal) {
                    *rect = QRect(origin.x() + position, origin.y(), size,
                                  header->viewport()->height());
                } else {
                    *rect = QRect(origin.x(), origin.y() + position,
                                  header->viewport()->width(), size);
                }
                return true;
            }
            return false;
        }
    }
    *rect = ref.rect;
    return !rect->isEmpty();
}

QJsonObject Server::virtualItem(const QString& id)
{
    const auto it = virtuals_.constFind(id);
    if (it == virtuals_.constEnd()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("unknown virtual item: %1").arg(id));
        return result;
    }
    const VirtualRef ref = it.value();
    if (ref.owner.isNull()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual item owner is gone: %1").arg(id));
        return result;
    }
    QRect rect;
    if (!virtualRect(ref, &rect)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual item is not visible: %1").arg(id));
        return result;
    }

    QJsonObject props = ref.props;
    if (ref.kind == QStringLiteral("qml_item")) {
        props.insert(QStringLiteral("qml_type"), ref.className);
        if (auto* qml = qobject_cast<QQuickItem*>(ref.qmlObject.data())) {
            props.insert(QStringLiteral("objectName"), qml->objectName());
            props.insert(QStringLiteral("visible"), qml->isVisible());
        }
    } else if (ref.kind == QStringLiteral("tab") && !ref.page.isNull()) {
        if (auto* tabs = qobject_cast<QTabWidget*>(ref.owner.data())) {
            const int index = tabs->indexOf(ref.page.data());
            if (index >= 0) {
                props.insert(QStringLiteral("text"), tabs->tabText(index));
                props.insert(QStringLiteral("enabled"), tabs->isTabEnabled(index));
                props.insert(QStringLiteral("selected"),
                             tabs->currentIndex() == index);
            }
        }
    } else if (ref.kind == QStringLiteral("spin_button")) {
        if (auto* spin = qobject_cast<QSpinBox*>(ref.owner.data())) {
            props.insert(QStringLiteral("value"), spin->value());
            props.insert(QStringLiteral("minimum"), spin->minimum());
            props.insert(QStringLiteral("maximum"), spin->maximum());
            props.insert(QStringLiteral("enabled"), spin->isEnabled());
        }
    } else if (ref.kind == QStringLiteral("groupbox_check")) {
        if (auto* group = qobject_cast<QGroupBox*>(ref.owner.data())) {
            props.insert(QStringLiteral("checked"), group->isChecked());
            props.insert(QStringLiteral("enabled"), group->isEnabled());
        }
    } else if (ref.kind == QStringLiteral("dock_button")) {
        if (auto* dock = qobject_cast<QDockWidget*>(ref.owner.data())) {
            props.insert(QStringLiteral("text"), dock->windowTitle());
            props.insert(QStringLiteral("enabled"), dock->isEnabled());
            props.insert(QStringLiteral("floating"), dock->isFloating());
        }
    } else if (ref.kind == QStringLiteral("toolbar_action") &&
               !ref.action.isNull()) {
        props.insert(QStringLiteral("text"), ref.action->text());
        props.insert(QStringLiteral("enabled"), ref.action->isEnabled());
        props.insert(QStringLiteral("checked"), ref.action->isChecked());
    } else if (ref.kind == QStringLiteral("menu_item") &&
               !ref.action.isNull()) {
        props.insert(QStringLiteral("text"), ref.action->text());
        props.insert(QStringLiteral("enabled"), ref.action->isEnabled());
        props.insert(QStringLiteral("checked"), ref.action->isChecked());
    } else if ((ref.kind == QStringLiteral("cell") ||
                ref.kind == QStringLiteral("tree_item") ||
                ref.kind == QStringLiteral("list_item") ||
                ref.kind == QStringLiteral("combo_item")) &&
               ref.persistent.isValid()) {
        const QVariant value = ref.persistent.data(Qt::DisplayRole);
        if (value.isValid())
            props.insert(QStringLiteral("text"), value.toString());
        props.insert(QStringLiteral("row"), ref.persistent.row());
        props.insert(QStringLiteral("column"), ref.persistent.column());
    }
    if (ref.kind == QStringLiteral("combo_item")) {
        if (auto* combo = qobject_cast<QComboBox*>(ref.owner.data())) {
            if (ref.index >= 0 && ref.index < combo->count()) {
                props.insert(QStringLiteral("text"), combo->itemText(ref.index));
                props.insert(QStringLiteral("selected"),
                             combo->currentIndex() == ref.index);
            }
        }
    }

    QJsonObject item;
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("virtual"), true);
    item.insert(QStringLiteral("kind"), ref.kind);
    item.insert(QStringLiteral("class"), ref.className);
    item.insert(QStringLiteral("parent"), idFor(ref.owner.data()));
    if (ref.kind == QStringLiteral("qml_item")) {
        item.insert(QStringLiteral("qml_type"), ref.className);
        QString qmlId;
        if (auto* qml = qobject_cast<QQuickItem*>(ref.qmlObject.data())) {
            if (QQmlContext* context = QQmlEngine::contextForObject(qml))
                qmlId = context->nameForObject(qml);
        }
        item.insert(QStringLiteral("qml_id"), qmlId);
        item.insert(QStringLiteral("objectName"),
                    ref.qmlObject ? ref.qmlObject->objectName() : QString());
    }
    QJsonObject r;
    r.insert(QStringLiteral("x"), rect.x());
    r.insert(QStringLiteral("y"), rect.y());
    r.insert(QStringLiteral("w"), rect.width());
    r.insert(QStringLiteral("h"), rect.height());
    item.insert(QStringLiteral("rect"), r);
    item.insert(QStringLiteral("props"), props);
    item.insert(QStringLiteral("child_count"), 0);
    item.insert(QStringLiteral("children"), QJsonArray{});
    item.insert(QStringLiteral("source"), QStringLiteral("adapter"));
    return item;
}

QJsonObject Server::actVirtual(const QJsonObject& params, const QString& id)
{
    const auto it = virtuals_.constFind(id);
    if (it == virtuals_.constEnd()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("unknown virtual item: %1").arg(id));
        return result;
    }
    const VirtualRef ref = it.value();
    if (ref.owner.isNull()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual item owner is gone"));
        return result;
    }
    if (!params.value(QStringLiteral("expect")).isNull() &&
        !params.value(QStringLiteral("expect")).isUndefined()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("expect is not implemented for virtual items"));
        return result;
    }
    const QString button =
        params.value(QStringLiteral("button")).toString(QStringLiteral("left"));
    if (button != QStringLiteral("left") ||
        !params.value(QStringLiteral("modifiers")).toArray().isEmpty()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual items only support plain left clicks"));
        return result;
    }
    if (params.value(QStringLiteral("nth")).toInt(-1) >= 0) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("nth is not meaningful for a virtual id"));
        return result;
    }
    const QString action = params.value(QStringLiteral("action")).toString();
    if (action != QStringLiteral("click") &&
        action != QStringLiteral("dblclick")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("only click/dblclick work on virtual items"));
        return result;
    }
    QRect rect;
    if (!virtualRect(ref, &rect)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual item is not visible"));
        return result;
    }

    if (ref.kind == QStringLiteral("spin_button")) {
        if (auto* spin = qobject_cast<QSpinBox*>(ref.owner.data())) {
            if (ref.index == 0)
                spin->stepUp();
            else
                spin->stepDown();
        }
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }
    if (ref.kind == QStringLiteral("toolbar_action")) {
        if (!ref.action.isNull())
            ref.action->trigger();
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }
    if (ref.kind == QStringLiteral("dock_button")) {
        if (auto* button = qobject_cast<QAbstractButton*>(ref.page.data()))
            button->click();
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }
    if (ref.kind == QStringLiteral("groupbox_check")) {
        if (auto* group = qobject_cast<QGroupBox*>(ref.owner.data()))
            group->setChecked(!group->isChecked());
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }
    if (ref.kind == QStringLiteral("combo_item")) {
        if (auto* combo = qobject_cast<QComboBox*>(ref.owner.data())) {
            if (ref.index >= 0 && ref.index < combo->count())
                combo->setCurrentIndex(ref.index);
        }
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }

    QWidget* target = ref.owner.data();
    QPoint local = rect.center();
    if (ref.kind == QStringLiteral("tab")) {
        if (auto* tabs = qobject_cast<QTabWidget*>(target))
            target = tabs->tabBar();
    } else if (ref.kind == QStringLiteral("combo_item") &&
               !ref.page.isNull()) {
        target = ref.page.data();
    } else if (ref.kind == QStringLiteral("splitter_handle") &&
               !ref.page.isNull()) {
        target = ref.page.data();
    }
    const QPoint global = target->mapToGlobal(local);
    const QPointer<QWidget> targetGuard(target);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(targetGuard.data(), &press);
    if (!targetGuard.isNull()) {
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local),
                            QPointF(global), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(targetGuard.data(), &release);
    }
    if (action == QStringLiteral("dblclick") && !targetGuard.isNull()) {
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(local),
                        QPointF(global), Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
        QApplication::sendEvent(targetGuard.data(), &dbl);
        if (!targetGuard.isNull()) {
            QMouseEvent lastRelease(QEvent::MouseButtonRelease, QPointF(local),
                                    QPointF(global), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(targetGuard.data(), &lastRelease);
        }
    }

    QJsonObject resolved;
    resolved.insert(QStringLiteral("id"), id);
    resolved.insert(QStringLiteral("class"), ref.className);
    resolved.insert(QStringLiteral("virtual"), true);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("action"), action);
    result.insert(QStringLiteral("dispatched"), true);
    result.insert(QStringLiteral("resolved"), resolved);
    result.insert(QStringLiteral("observed"), resolved);
    return result;
}

QJsonObject Server::source(const QJsonObject& params)
{
    const QJsonValue sourceTarget = params.value(QStringLiteral("target"));
    QString sourceVirtualId;
    if (sourceTarget.isString()) {
        sourceVirtualId = sourceTarget.toString();
    } else if (sourceTarget.isObject()) {
        sourceVirtualId =
            sourceTarget.toObject().value(QStringLiteral("id")).toString();
    }
    if (!sourceVirtualId.isEmpty() && virtuals_.contains(sourceVirtualId)) {
        const VirtualRef ref = virtuals_.value(sourceVirtualId);
        auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        if (ref.kind == QStringLiteral("qml_item") && item) {
            UiHit hit;
            const QString queryName =
                !item->objectName().isEmpty()
                    ? item->objectName()
                    : (QQmlEngine::contextForObject(item)
                           ? QQmlEngine::contextForObject(item)
                                 ->nameForObject(item)
                           : QString());
            const bool located =
                !queryName.isEmpty() && findQmlLocation(queryName, &hit);
            QJsonObject target;
            target.insert(QStringLiteral("id"), sourceVirtualId);
            target.insert(QStringLiteral("class"), ref.className);
            target.insert(QStringLiteral("objectName"), item->objectName());
            target.insert(QStringLiteral("qml_id"),
                          QQmlEngine::contextForObject(item)
                              ? QQmlEngine::contextForObject(item)
                                    ->nameForObject(item)
                              : QString());
            QJsonObject location;
            location.insert(
                QStringLiteral("url"),
                located ? QUrl::fromLocalFile(hit.file).toString()
                        : QJsonValue(QJsonValue::Null));
            location.insert(
                QStringLiteral("file"),
                located ? QJsonValue(hit.file) : QJsonValue(QJsonValue::Null));
            location.insert(QStringLiteral("line"), located ? hit.line : 0);
            location.insert(QStringLiteral("column"), 0);
            location.insert(QStringLiteral("origin"),
                            located ? QStringLiteral("qml")
                                    : QStringLiteral("none"));
            location.insert(QStringLiteral("confidence"),
                            located ? QStringLiteral("medium")
                                    : QStringLiteral("low"));
            location.insert(QStringLiteral("stale"), false);
            QJsonObject qml;
            qml.insert(QStringLiteral("type"), ref.className);
            qml.insert(QStringLiteral("objectName"), item->objectName());
            qml.insert(QStringLiteral("id"),
                       QQmlEngine::contextForObject(item)
                           ? QQmlEngine::contextForObject(item)
                                 ->nameForObject(item)
                           : QString());
            QJsonArray diagnostics;
            if (!located)
                diagnostics.append(QStringLiteral(
                    "No .qml source with this QML objectName was found."));
            QJsonObject result;
            result.insert(QStringLiteral("available"), located);
            if (!located)
                result.insert(QStringLiteral("code"),
                              QStringLiteral("E_NO_DEBUGINFO"));
            result.insert(QStringLiteral("kind"), QStringLiteral("qml"));
            result.insert(QStringLiteral("target"), target);
            result.insert(QStringLiteral("location"), location);
            result.insert(QStringLiteral("qml"), qml);
            result.insert(QStringLiteral("diagnostics"), diagnostics);
            return result;
        }
    }

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        params.value(QStringLiteral("nth")).toInt(-1),
        params.value(QStringLiteral("include_hidden")).toBool(false));
    if (!widget) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), resolveErrorCode());
        result.insert(QStringLiteral("message"), error);
        return result;
    }

    QJsonObject target;
    target.insert(QStringLiteral("id"), idFor(widget));
    target.insert(QStringLiteral("class"),
                  QString::fromLatin1(widget->metaObject()->className()));
    target.insert(QStringLiteral("objectName"), widget->objectName());

    UiHit hit;
    QString origin = QStringLiteral("none");
    bool located = findUiLocation(widget->objectName(), &hit);
    if (located) {
        origin = QStringLiteral("ui");
    } else {
        const QVariant root = widget->property("rootObject");
        if (auto* object = objectFromVariant(root)) {
            located = findQmlLocation(object->objectName(), &hit);
            if (located)
                origin = QStringLiteral("qml");
        }
        if (!located) {
            if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
                if (QQuickItem* qmlRoot = quick->rootObject()) {
                    located = findQmlLocation(qmlRoot->objectName(), &hit);
                    if (located)
                        origin = QStringLiteral("qml");
                }
            }
        }
    }
    QJsonObject location;
    location.insert(QStringLiteral("url"),
                    located ? QUrl::fromLocalFile(hit.file).toString()
                            : QJsonValue(QJsonValue::Null));
    location.insert(QStringLiteral("file"),
                    located ? QJsonValue(hit.file) : QJsonValue(QJsonValue::Null));
    location.insert(QStringLiteral("line"), located ? hit.line : 0);
    location.insert(QStringLiteral("column"), 0);
    location.insert(QStringLiteral("origin"), origin);
    location.insert(QStringLiteral("confidence"),
                    located ? QStringLiteral("medium") : QStringLiteral("low"));
    location.insert(QStringLiteral("stale"), false);

    QJsonObject widgetInfo;
    widgetInfo.insert(QStringLiteral("className"),
                      QString::fromLatin1(widget->metaObject()->className()));
    widgetInfo.insert(QStringLiteral("objectName"), widget->objectName());

    QJsonArray diagnostics;
    if (!located) {
        diagnostics.append(QStringLiteral(
            "No .ui/.qml source with this objectName was found near the "
            "executable or source root; this build exposes identity only."));
    } else {
        diagnostics.append(QStringLiteral(
            "Matched a %1 source by objectName (heuristic).").arg(origin));
    }

    QJsonObject result;
    result.insert(QStringLiteral("available"), located);
    if (!located)
        result.insert(QStringLiteral("code"), QStringLiteral("E_NO_DEBUGINFO"));
    result.insert(QStringLiteral("kind"), QStringLiteral("widget"));
    result.insert(QStringLiteral("target"), target);
    result.insert(QStringLiteral("location"), location);
    result.insert(QStringLiteral("widget"), widgetInfo);
    result.insert(QStringLiteral("diagnostics"), diagnostics);
    return result;
}

bool Server::itemMatches(const QJsonObject& props, const QString& kind,
                           const QString& cls,
                           const QJsonObject& where) const
{
    static const QSet<QString> allowed{
        QStringLiteral("kind"), QStringLiteral("class"),
        QStringLiteral("text"), QStringLiteral("props")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    if (where.contains(QStringLiteral("kind")) &&
        where.value(QStringLiteral("kind")).toString() != kind)
        return false;
    if (where.contains(QStringLiteral("class")) &&
        where.value(QStringLiteral("class")).toString() != cls)
        return false;
    if (where.contains(QStringLiteral("text"))) {
        if (props.value(QStringLiteral("text")).toString() !=
            where.value(QStringLiteral("text")).toString())
            return false;
    }
    if (where.contains(QStringLiteral("props"))) {
        const QJsonObject expected = where.value(QStringLiteral("props")).toObject();
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (props.value(it.key()) != it.value())
                return false;
        }
    }
    return true;
}

QJsonObject Server::nodeFor(QWidget* widget, const QJsonValue& props)
{
    QueryContext context;
    context.props = props;
    context.maxDepth = 0;
    bool any = false;
    return buildNode(widget, context, 0, false, &any);
}

QWidget* Server::resolveTarget(const QJsonValue& target, QString* error,
                               int nth, bool includeHidden)
{
    lastResolveCode_ = QStringLiteral("E_NOT_FOUND");
    auto byId = [&](const QString& id) -> QWidget* {
        if (staleIds_.contains(id) || idToWidget_.contains(id)) {
            const QPointer<QWidget> widget = idToWidget_.value(id);
            if (!widget.isNull()) {
                lastResolveCode_ = QStringLiteral("E_OK");
                return widget.data();
            }
            lastResolveCode_ = QStringLiteral("E_STALE_ID");
            if (error)
                *error = QStringLiteral("stale id: %1").arg(id);
            return nullptr;
        }
        if (error)
            *error = QStringLiteral("unknown id: %1").arg(id);
        return nullptr;
    };

    if (target.isString())
        return byId(target.toString());
    if (!target.isObject()) {
        lastResolveCode_ = QStringLiteral("E_INVALID_ARGUMENT");
        if (error)
            *error = QStringLiteral("target must be an id or a locator");
        return nullptr;
    }

    const QJsonObject where = target.toObject();
    if (where.isEmpty()) {
        lastResolveCode_ = QStringLiteral("E_INVALID_ARGUMENT");
        if (error)
            *error = QStringLiteral("target object is empty");
        return nullptr;
    }
    if (where.contains(QStringLiteral("id")))
        return byId(where.value(QStringLiteral("id")).toString());

    static const QSet<QString> allowed{
        QStringLiteral("class"), QStringLiteral("object_name"),
        QStringLiteral("objectName"), QStringLiteral("text"),
        QStringLiteral("props")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key())) {
            lastResolveCode_ = QStringLiteral("E_INVALID_FILTER");
            if (error)
                *error = QStringLiteral(
                             "unsupported locator key: %1 (supported: id, "
                             "class, object_name/objectName, text, props)")
                             .arg(it.key());
            return nullptr;
        }
        if (it.key() == QStringLiteral("props")) {
            if (!it.value().isObject()) {
                lastResolveCode_ = QStringLiteral("E_INVALID_FILTER");
                if (error)
                    *error = QStringLiteral("props filter must be an object");
                return nullptr;
            }
        } else if (!validMatcher(it.value())) {
            lastResolveCode_ = QStringLiteral("E_INVALID_FILTER");
            if (error)
                *error = QStringLiteral(
                             "invalid matcher for %1 (use string, wildcard, "
                             "or contains/startsWith/regex/equals)")
                             .arg(it.key());
            return nullptr;
        }
    }

    const QList<QWidget*> matches = collectMatches(where, includeHidden);
    if (matches.isEmpty()) {
        if (error)
            *error = QStringLiteral("no widget matches target");
        return nullptr;
    }
    if (nth < 0) {
        if (matches.size() > 1) {
            lastResolveCode_ = QStringLiteral("E_AMBIGUOUS");
            if (error)
                *error = QStringLiteral("%1 widgets match; provide nth (0-based)")
                             .arg(matches.size());
            return nullptr;
        }
        lastResolveCode_ = QStringLiteral("E_OK");
        return matches.first();
    }
    if (nth >= matches.size()) {
        lastResolveCode_ = QStringLiteral("E_AMBIGUOUS");
        if (error)
            *error = QStringLiteral("nth %1 out of range (0..%2)")
                         .arg(nth)
                         .arg(matches.size() - 1);
        return nullptr;
    }
    lastResolveCode_ = QStringLiteral("E_OK");
    return matches.at(nth);
}

QString Server::resolveErrorCode() const
{
    return lastResolveCode_.isEmpty() ? QStringLiteral("E_NOT_FOUND")
                                      : lastResolveCode_;
}

QList<QWidget*> Server::collectMatches(const QJsonObject& where,
                                        bool includeHidden) const
{
    QList<QWidget*> roots = QApplication::topLevelWidgets();
    std::sort(roots.begin(), roots.end(), [](QWidget* left, QWidget* right) {
        return left->winId() < right->winId();
    });
    QList<QWidget*> found;
    QSet<QWidget*> visited;
    std::function<void(QWidget*)> walk = [&](QWidget* widget) {
        if (!widget || (!includeHidden && !widget->isVisible()) ||
            visited.contains(widget))
            return;
        visited.insert(widget);
        if (matches(widget, where))
            found.append(widget);
        const QObjectList children = widget->children();
        for (QObject* object : children) {
            if (auto* child = qobject_cast<QWidget*>(object))
                walk(child);
        }
    };
    for (QWidget* root : roots)
        walk(root);
    return found;
}

QWidget* Server::findByWhere(const QJsonObject& where) const
{
    const QList<QWidget*> matches = collectMatches(where);
    return matches.isEmpty() ? nullptr : matches.first();
}

QWidget* Server::findIn(QWidget* root, const QJsonObject& where) const
{
    if (matches(root, where))
        return root;
    const QObjectList children = root->children();
    for (QObject* object : children) {
        auto* child = qobject_cast<QWidget*>(object);
        if (!child || !child->isVisible())
            continue;
        if (QWidget* found = findIn(child, where))
            return found;
    }
    return nullptr;
}

namespace {

QString qmlIdName(const QQuickItem* item)
{
    QQmlContext* context = QQmlEngine::contextForObject(item);
    return context ? context->nameForObject(const_cast<QQuickItem*>(item))
                   : QString();
}

QString qmlTypeName(const QQuickItem* item)
{
    QString name = QString::fromLatin1(item->metaObject()->className());
    name.replace(QRegularExpression(QStringLiteral("_QMLTYPE_\\d+")),
                 QString());
    name.replace(QRegularExpression(QStringLiteral("_QML_\\d+")), QString());
    return name;
}

QRect qmlItemRect(const QQuickItem* item, const QQuickWidget* owner)
{
    if (!item || !owner)
        return {};
    const QQuickItem* root = owner->rootObject();
    const QRectF rect =
        root ? item->mapRectToItem(root, item->boundingRect())
             : item->boundingRect();
    return rect.toAlignedRect();
}

bool qmlMatches(const QQuickItem* item, const QJsonObject& where)
{
    static const QSet<QString> allowed{
        QStringLiteral("class"), QStringLiteral("object_name"),
        QStringLiteral("objectName"), QStringLiteral("qml_id"),
        QStringLiteral("qml_type"), QStringLiteral("text"),
        QStringLiteral("props")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    const QString className =
        QString::fromLatin1(item->metaObject()->className());
    if (where.contains(QStringLiteral("class")) &&
        !matchText(where.value(QStringLiteral("class")), className))
        return false;
    const QJsonValue objectNameSpec =
        where.contains(QStringLiteral("object_name"))
            ? where.value(QStringLiteral("object_name"))
            : where.value(QStringLiteral("objectName"));
    if ((where.contains(QStringLiteral("object_name")) ||
         where.contains(QStringLiteral("objectName"))) &&
        !matchText(objectNameSpec, item->objectName()))
        return false;
    if (where.contains(QStringLiteral("qml_id")) &&
        !matchText(where.value(QStringLiteral("qml_id")), qmlIdName(item)))
        return false;
    if (where.contains(QStringLiteral("qml_type")) &&
        !matchText(where.value(QStringLiteral("qml_type")), qmlTypeName(item)))
        return false;
    if (where.contains(QStringLiteral("text")) &&
        !matchText(where.value(QStringLiteral("text")),
                   item->property("text").toString()))
        return false;
    if (where.contains(QStringLiteral("props"))) {
        const QJsonObject props = where.value(QStringLiteral("props")).toObject();
        for (auto it = props.begin(); it != props.end(); ++it) {
            const QVariant value =
                item->property(it.key().toUtf8().constData());
            if (!value.isValid() ||
                QJsonValue::fromVariant(value) != it.value())
                return false;
        }
    }
    return true;
}

QJsonObject qmlProps(const QQuickItem* item, const QJsonValue& requested)
{
    QJsonObject props;
    if (requested.isString() && requested.toString() == QStringLiteral("*")) {
        const QMetaObject* meta = item->metaObject();
        for (int i = 0; i < meta->propertyCount() && props.size() < 64; ++i) {
            const QMetaProperty property = meta->property(i);
            if (!property.isReadable())
                continue;
            props.insert(QString::fromLatin1(property.name()),
                         QJsonValue::fromVariant(property.read(item)));
        }
    } else if (requested.isArray()) {
        for (const QJsonValue& value : requested.toArray()) {
            const QString name = value.toString();
            const QVariant valueData = item->property(name.toUtf8().constData());
            if (valueData.isValid())
                props.insert(name, QJsonValue::fromVariant(valueData));
        }
    }
    return props;
}

} // namespace

QString Server::registerQml(QWidget* owner, QQuickItem* item, int index,
                            const QRect& rect, const QJsonObject& props)
{
    const QString id =
        tag_ + QStringLiteral(".vqml.") + idFor(owner) +
        QStringLiteral(".") +
        QString::number(reinterpret_cast<quintptr>(item), 16);
    VirtualRef ref;
    ref.owner = owner;
    ref.kind = QStringLiteral("qml_item");
    ref.className = qmlTypeName(item);
    ref.index = index;
    ref.rect = rect;
    ref.props = props;
    ref.qmlObject = item;
    virtuals_.insert(id, ref);
    return id;
}

QJsonObject Server::buildQmlNode(QWidget* owner, QQuickItem* item,
                                 QueryContext& context, int depth, int* serial,
                                 bool* anyMatch)
{
    *anyMatch = false;
    if (!item)
        return {};
    const bool selfMatch =
        context.hasWhere && qmlMatches(item, context.where);
    ++context.total;
    if (selfMatch)
        ++context.matched;

    const QRect rect = qmlItemRect(item, qobject_cast<QQuickWidget*>(owner));
    const QJsonObject props = qmlProps(item, context.props);
    int childCount = 0;
    QJsonArray children;
    bool childAny = false;
    for (QQuickItem* child : item->childItems()) {
        if (!child || !child->isVisible())
            continue;
        ++childCount;
        if (context.maxDepth >= 0 && depth >= context.maxDepth)
            continue;
        bool any = false;
        const QJsonObject childNode =
            buildQmlNode(owner, child, context, depth + 1, serial, &any);
        if (any) {
            children.append(childNode);
            childAny = true;
        }
    }
    *anyMatch = context.hasWhere ? (selfMatch || childAny) : true;
    if (context.hasWhere && !*anyMatch)
        return {};
    if (context.emitted >= QueryContext::kMaxEmitted) {
        context.tooLarge = true;
        return {};
    }
    ++context.emitted;

    QJsonObject node;
    node.insert(QStringLiteral("id"),
                registerQml(owner, item, (*serial)++, rect, props));
    node.insert(QStringLiteral("class"), qmlTypeName(item));
    node.insert(QStringLiteral("qml_type"), qmlTypeName(item));
    node.insert(QStringLiteral("qml_id"), qmlIdName(item));
    node.insert(QStringLiteral("objectName"), item->objectName());
    node.insert(QStringLiteral("qml"), true);
    node.insert(QStringLiteral("window"), false);
    QJsonObject rectObject;
    rectObject.insert(QStringLiteral("x"), rect.x());
    rectObject.insert(QStringLiteral("y"), rect.y());
    rectObject.insert(QStringLiteral("w"), rect.width());
    rectObject.insert(QStringLiteral("h"), rect.height());
    node.insert(QStringLiteral("rect"), rectObject);
    if (context.screenCoords) {
        const QPoint screen = owner->mapToGlobal(rect.topLeft());
        QJsonObject screenRect;
        screenRect.insert(QStringLiteral("x"), screen.x());
        screenRect.insert(QStringLiteral("y"), screen.y());
        screenRect.insert(QStringLiteral("w"), rect.width());
        screenRect.insert(QStringLiteral("h"), rect.height());
        node.insert(QStringLiteral("screen_rect"), screenRect);
    }
    node.insert(QStringLiteral("child_count"), childCount);
    node.insert(QStringLiteral("children"), children);
    node.insert(QStringLiteral("props"), props);
    if (context.hasWhere)
        node.insert(QStringLiteral("matched"), selfMatch);
    return node;
}

QJsonObject Server::buildNode(QWidget* widget, QueryContext& context,
                                int depth, bool isRoot, bool* anyMatch)
{
    *anyMatch = false;
    if (!widget)
        return {};

    const bool selfMatch =
        context.hasWhere && matches(widget, context.where);
    ++context.total;
    if (selfMatch)
        ++context.matched;

    const QObjectList children = widget->children();
    int childCount = 0;
    QJsonArray childNodes;
    bool childAny = false;
    for (QObject* object : children) {
        auto* child = qobject_cast<QWidget*>(object);
        if (!child || !isCandidate(child, context.includeHidden))
            continue;
        ++childCount;
        if (context.maxDepth >= 0 && depth >= context.maxDepth)
            continue;
        bool any = false;
        const QJsonObject childNode =
            buildNode(child, context, depth + 1, false, &any);
        if (any) {
            childNodes.append(childNode);
            childAny = true;
        }
    }

    if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
        if (QQuickItem* qmlRoot = quick->rootObject()) {
            int serial = 0;
            bool any = false;
            const QJsonObject qmlNode =
                buildQmlNode(widget, qmlRoot, context, depth + 1, &serial, &any);
            if (any) {
                childNodes.append(qmlNode);
                ++childCount;
                childAny = true;
            }
        }
    }

    *anyMatch = context.hasWhere ? (selfMatch || childAny) : true;
    if (context.hasWhere && !*anyMatch)
        return {};
    if (context.emitted >= QueryContext::kMaxEmitted) {
        context.tooLarge = true;
        return {};
    }
    ++context.emitted;

    QJsonObject node;
    node.insert(QStringLiteral("id"), idFor(widget));
    node.insert(QStringLiteral("class"),
                QString::fromLatin1(widget->metaObject()->className()));
    node.insert(QStringLiteral("qml_type"), QJsonValue::Null);
    node.insert(QStringLiteral("objectName"), widget->objectName());
    node.insert(QStringLiteral("qml_id"), QStringLiteral(""));

    const bool window = widget->isWindow();
    QJsonObject rect;
    if (isRoot || window) {
        rect.insert(QStringLiteral("x"), 0);
        rect.insert(QStringLiteral("y"), 0);
    } else {
        rect.insert(QStringLiteral("x"), widget->x());
        rect.insert(QStringLiteral("y"), widget->y());
    }
    rect.insert(QStringLiteral("w"), widget->width());
    rect.insert(QStringLiteral("h"), widget->height());
    node.insert(QStringLiteral("rect"), rect);
    node.insert(QStringLiteral("window"), window);
    if (window)
        node.insert(QStringLiteral("minimized"), widget->isMinimized());

    if (isRoot || window) {
        const QPoint screen = widget->mapToGlobal(QPoint(0, 0));
        QJsonObject screenPos;
        screenPos.insert(QStringLiteral("x"), screen.x());
        screenPos.insert(QStringLiteral("y"), screen.y());
        node.insert(QStringLiteral("screen_pos"), screenPos);
    }
    if (context.screenCoords) {
        const QPoint screen = widget->mapToGlobal(QPoint(0, 0));
        QJsonObject screenRect;
        screenRect.insert(QStringLiteral("x"), screen.x());
        screenRect.insert(QStringLiteral("y"), screen.y());
        screenRect.insert(QStringLiteral("w"), widget->width());
        screenRect.insert(QStringLiteral("h"), widget->height());
        node.insert(QStringLiteral("screen_rect"), screenRect);
    }

    if (context.hasWhere)
        node.insert(QStringLiteral("matched"), selfMatch);

    addProps(&node, widget, context.props);
    node.insert(QStringLiteral("child_count"), childCount);
    node.insert(QStringLiteral("children"), childNodes);
    return node;
}

void Server::countNode(QWidget* widget, QueryContext& context, int depth)
{
    if (!widget)
        return;
    ++context.total;
    if (context.hasWhere && matches(widget, context.where))
        ++context.matched;
    if (context.maxDepth >= 0 && depth >= context.maxDepth)
        return;
    const QObjectList children = widget->children();
    for (QObject* object : children) {
        auto* child = qobject_cast<QWidget*>(object);
        if (!child || !isCandidate(child, context.includeHidden))
            continue;
        countNode(child, context, depth + 1);
    }
    if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
        std::function<void(QQuickItem*, int)> countQml =
            [&](QQuickItem* item, int qmlDepth) {
                if (!item || !item->isVisible())
                    return;
                ++context.total;
                if (context.hasWhere && qmlMatches(item, context.where))
                    ++context.matched;
                if (context.maxDepth >= 0 && qmlDepth >= context.maxDepth)
                    return;
                for (QQuickItem* child : item->childItems())
                    countQml(child, qmlDepth + 1);
            };
        countQml(quick->rootObject(), depth + 1);
    }
}

bool Server::matches(QWidget* widget, const QJsonObject& where) const
{
    static const QSet<QString> allowed{
        QStringLiteral("id"), QStringLiteral("class"),
        QStringLiteral("object_name"), QStringLiteral("objectName"),
        QStringLiteral("qml_id"), QStringLiteral("qml_type"),
        QStringLiteral("text"), QStringLiteral("props")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    if (where.contains(QStringLiteral("id"))) {
        const auto found = ids_.constFind(widget);
        if (found == ids_.constEnd() ||
            found.value() != where.value(QStringLiteral("id")).toString())
            return false;
    }
    const QString className =
        QString::fromLatin1(widget->metaObject()->className());
    if (where.contains(QStringLiteral("class")) &&
        !matchText(where.value(QStringLiteral("class")), className))
        return false;
    const QJsonValue objectNameSpec =
        where.contains(QStringLiteral("object_name"))
            ? where.value(QStringLiteral("object_name"))
            : where.value(QStringLiteral("objectName"));
    if ((where.contains(QStringLiteral("object_name")) ||
         where.contains(QStringLiteral("objectName"))) &&
        !matchText(objectNameSpec, widget->objectName()))
        return false;
    if (where.contains(QStringLiteral("qml_id")) ||
        where.contains(QStringLiteral("qml_type")))
        return false;
    if (where.contains(QStringLiteral("text")) &&
        !matchText(where.value(QStringLiteral("text")),
                   widget->property("text").toString()))
        return false;
    if (where.contains(QStringLiteral("props"))) {
        const QJsonObject props =
            where.value(QStringLiteral("props")).toObject();
        for (auto it = props.begin(); it != props.end(); ++it) {
            const QVariant value =
                widget->property(it.key().toUtf8().constData());
            if (!value.isValid())
                return false;
            if (QJsonValue::fromVariant(value) != it.value())
                return false;
        }
    }
    return true;
}

void Server::addProps(QJsonObject* node, QWidget* widget,
                        const QJsonValue& spec) const
{
    if (!spec.isArray() && !spec.isString())
        return;
    QStringList names;
    if (spec.isString() && spec.toString() != QStringLiteral("*")) {
        names = spec.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& name : names) {
            const QVariant value = widget->property(name.toUtf8().constData());
            if (value.isValid())
                node->insert(name, QJsonValue::fromVariant(value));
        }
        return;
    }
    if (spec.isString()) {
        QSet<QString> seen;
        const QMetaObject* meta = widget->metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty property = meta->property(i);
            if (!property.isReadable())
                continue;
            const QString name = QString::fromLatin1(property.name());
            if (!seen.contains(name)) {
                seen.insert(name);
                names.append(name);
            }
        }
        for (const QByteArray& raw : widget->dynamicPropertyNames()) {
            const QString name = QString::fromUtf8(raw);
            if (!seen.contains(name)) {
                seen.insert(name);
                names.append(name);
            }
        }
    } else {
        for (const QJsonValue& value : spec.toArray()) {
            if (value.isString())
                names.append(value.toString());
        }
    }

    const bool wildcard = spec.isString() && spec.toString() == QStringLiteral("*");
    if (wildcard && names.size() > 64) {
        node->insert(QStringLiteral("props_truncated"), true);
        node->insert(QStringLiteral("props_total"), names.size());
        names = names.mid(0, 64);
    }

    QJsonObject props;
    for (const QString& name : names) {
        const QVariant value =
            widget->property(name.toUtf8().constData());
        if (!value.isValid())
            continue;
        const int type = value.userType();
        if (type == QMetaType::QObjectStar || type == QMetaType::QPixmap ||
            type == QMetaType::QImage || type == QMetaType::QIcon)
            continue;
        props.insert(name, QJsonValue::fromVariant(value));
    }
    node->insert(QStringLiteral("props"), props);
}

QString Server::idFor(QWidget* widget)
{
    auto it = ids_.find(widget);
    if (it != ids_.end()) {
        if (!idToWidget_.value(it.value()).isNull())
            return it.value();
        // The QPointer went null without a destroyed() handler (defensive).
        staleIds_.insert(it.value());
        idToWidget_.remove(it.value());
        ids_.erase(it);
    }

    const QString id = tag_ + QStringLiteral(".") + QString::number(nextId_++);
    ids_.insert(widget, id);
    idToWidget_.insert(id, QPointer<QWidget>(widget));
    connect(widget, &QObject::destroyed, this, [this, id, widget]() {
        ids_.remove(widget);
        idToWidget_.remove(id);
        staleIds_.insert(id);
    });
    return id;
}

} // namespace qt_commander


