#include "server.h"
#include "qt_commander/version.h"

#include <QAbstractButton>
#include <QAbstractEventDispatcher>
#include <QAbstractItemView>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QDockWidget>
#include <QHeaderView>
#include <QHoverEvent>
#include <QImageWriter>
#include <QJsonArray>
#include <QJSValue>
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
#include <QTimer>
#include <QTreeView>
#include <QWheelEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QWindow>
#include <QGuiApplication>
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
QString qmlTextContent(QQuickItem* item);
bool qmlActionable(QQuickItem* item);

constexpr const char* kLibraryVersion = QT_COMMANDER_VERSION;

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
    return operators.contains(object.begin().key()) &&
           object.begin().value().isString();
}

struct QmlItemMatch {
    QQuickWindow* window = nullptr;
    QQuickItem* item = nullptr;
};

QList<QmlItemMatch> findQmlMatches(const QString& qmlId)
{
    QList<QmlItemMatch> matches;
    if (qmlId.isEmpty())
        return matches;
    std::function<void(QQuickWindow*, QQuickItem*)> walk =
        [&](QQuickWindow* window, QQuickItem* item) {
            if (!item)
                return;
            QQmlContext* context = QQmlEngine::contextForObject(item);
            if (context && context->nameForObject(item) == qmlId)
                matches.append({window, item});
            for (QQuickItem* child : item->childItems())
                walk(window, child);
        };
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (auto* quick = qobject_cast<QQuickWindow*>(window))
            walk(quick, quick->contentItem());
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const QmlItemMatch& left, const QmlItemMatch& right) {
                         if (left.item->isVisible() != right.item->isVisible())
                             return left.item->isVisible();
                         return left.item->width() * left.item->height() >
                                right.item->width() * right.item->height();
                     });
    return matches;
}

QJsonArray qmlCandidates(const QList<QmlItemMatch>& matches)
{
    QJsonArray candidates;
    for (int i = 0; i < matches.size(); ++i) {
        const QmlItemMatch& match = matches.at(i);
        const QRect rect =
            match.item->mapRectToScene(match.item->boundingRect()).toAlignedRect();
        QJsonObject candidate;
        candidate.insert(QStringLiteral("nth"), i);
        candidate.insert(QStringLiteral("visible"), match.item->isVisible());
        candidate.insert(QStringLiteral("class"),
                         QString::fromLatin1(
                             match.item->metaObject()->className()));
        candidate.insert(QStringLiteral("qml_id"),
                         qmlIdName(match.item));
        candidate.insert(QStringLiteral("objectName"),
                         match.item->objectName());
        candidate.insert(QStringLiteral("text"),
                         qmlTextContent(match.item));
        candidate.insert(QStringLiteral("actionable"),
                         qmlActionable(match.item));
        QStringList pathParts;
        for (QQuickItem* parent = match.item; parent;
             parent = parent->parentItem()) {
            QString part = qmlIdName(parent);
            if (part.isEmpty())
                part = parent->objectName();
            if (part.isEmpty())
                part = qmlTypeName(parent);
            pathParts.prepend(part);
        }
        candidate.insert(QStringLiteral("path"),
                         pathParts.join(QStringLiteral("/")));
        candidate.insert(QStringLiteral("rect"), QJsonObject{
            {QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("w"), rect.width()},
            {QStringLiteral("h"), rect.height()}});
        if (match.window) {
            const QPoint screen = match.window->mapToGlobal(rect.topLeft());
            candidate.insert(QStringLiteral("screen_rect"), QJsonObject{
                {QStringLiteral("x"), screen.x()},
                {QStringLiteral("y"), screen.y()},
                {QStringLiteral("w"), rect.width()},
                {QStringLiteral("h"), rect.height()}});
        }
        candidates.append(candidate);
    }
    return candidates;
}

bool matchText(const QJsonValue& spec, const QString& actual);

QString qmlTextContent(QQuickItem* item)
{
    if (!item)
        return {};
    const QString own = item->property("text").toString();
    if (!own.isEmpty())
        return own;
    for (QQuickItem* child : item->childItems()) {
        const QString nested = qmlTextContent(child);
        if (!nested.isEmpty())
            return nested;
    }
    return {};
}

bool qmlMatchesTarget(QQuickItem* item, const QJsonObject& object)
{
    if (!item)
        return false;
    if (object.contains(QStringLiteral("class")) &&
        !matchText(object.value(QStringLiteral("class")),
                   QString::fromLatin1(item->metaObject()->className())))
        return false;
    if (object.contains(QStringLiteral("qml_type")) &&
        !matchText(object.value(QStringLiteral("qml_type")),
                   QString::fromLatin1(item->metaObject()->className())))
        return false;
    const QJsonValue objectSpec =
        object.contains(QStringLiteral("object_name"))
            ? object.value(QStringLiteral("object_name"))
            : object.value(QStringLiteral("objectName"));
    if ((object.contains(QStringLiteral("object_name")) ||
         object.contains(QStringLiteral("objectName"))) &&
        !matchText(objectSpec, item->objectName()))
        return false;
    if (object.contains(QStringLiteral("text")) &&
        !matchText(object.value(QStringLiteral("text")),
                   qmlTextContent(item)))
        return false;
    if (object.contains(QStringLiteral("props"))) {
        const QJsonObject props =
            object.value(QStringLiteral("props")).toObject();
        for (auto it = props.begin(); it != props.end(); ++it) {
            const QVariant value =
                item->property(it.key().toUtf8().constData());
            if (!value.isValid() || QJsonValue::fromVariant(value) != it.value())
                return false;
        }
    }
    return true;
}

bool qmlActionable(QQuickItem* item)
{
    if (!item)
        return false;
    if ((item->acceptedMouseButtons() & Qt::LeftButton) ||
        item->inherits("QQuickAbstractButton") ||
        item->inherits("QQuickMouseArea"))
        return true;
    for (QQuickItem* child : item->childItems()) {
        if (qmlActionable(child))
            return true;
    }
    return false;
}

QQuickItem* qmlActionableAncestor(QQuickItem* item)
{
    for (QQuickItem* parent = item ? item->parentItem() : nullptr; parent;
         parent = parent->parentItem()) {
        if (qmlActionable(parent))
            return parent;
    }
    return nullptr;
}

bool qmlContainsItem(QQuickItem* ancestor, QQuickItem* item)
{
    for (QQuickItem* parent = item ? item->parentItem() : nullptr; parent;
         parent = parent->parentItem()) {
        if (parent == ancestor)
            return true;
    }
    return false;
}

bool resolveQmlTarget(const QJsonValue& target, int nth,
                      QQuickWindow** window, QQuickItem** item,
                      QString* error, QJsonArray* candidates = nullptr,
                      bool includeHidden = false)
{
    QJsonObject object;
    if (target.isString())
        object.insert(QStringLiteral("qml_id"), target.toString());
    else if (target.isObject())
        object = target.toObject();
    else
        return false;
    const QString qmlId = object.value(QStringLiteral("qml_id")).toString();
    QList<QmlItemMatch> matches;
    if (!qmlId.isEmpty()) {
        matches = findQmlMatches(qmlId);
        if (object.size() > 1) {
            QList<QmlItemMatch> filtered;
            for (const QmlItemMatch& match : matches) {
                if (qmlMatchesTarget(match.item, object))
                    filtered.append(match);
            }
            matches = filtered;
        }
    } else {
        static const QSet<QString> matcherKeys{
            QStringLiteral("class"), QStringLiteral("object_name"),
            QStringLiteral("objectName"), QStringLiteral("text"),
            QStringLiteral("qml_type"), QStringLiteral("props")};
        bool hasMatcher = false;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (matcherKeys.contains(it.key())) {
                hasMatcher = true;
                break;
            }
        }
        if (!hasMatcher)
            return false;
        auto textMatches = [&](QQuickItem* candidate) {
            if (object.contains(QStringLiteral("class")) &&
                !matchText(object.value(QStringLiteral("class")),
                           QString::fromLatin1(
                               candidate->metaObject()->className())))
                return false;
            if (object.contains(QStringLiteral("qml_type")) &&
                !matchText(object.value(QStringLiteral("qml_type")),
                           QString::fromLatin1(
                               candidate->metaObject()->className())))
                return false;
            const QJsonValue objectSpec =
                object.contains(QStringLiteral("object_name"))
                    ? object.value(QStringLiteral("object_name"))
                    : object.value(QStringLiteral("objectName"));
            if ((object.contains(QStringLiteral("object_name")) ||
                 object.contains(QStringLiteral("objectName"))) &&
                !matchText(objectSpec, candidate->objectName()))
                return false;
            if (object.contains(QStringLiteral("text")) &&
                !matchText(object.value(QStringLiteral("text")),
                           candidate->property("text").toString()))
                return false;
            if (object.contains(QStringLiteral("props"))) {
                const QJsonObject props =
                    object.value(QStringLiteral("props")).toObject();
                for (auto it = props.begin(); it != props.end(); ++it) {
                    const QVariant value =
                        candidate->property(it.key().toUtf8().constData());
                    if (!value.isValid() ||
                        QJsonValue::fromVariant(value) != it.value())
                        return false;
                }
            }
            return true;
        };
        std::function<void(QQuickWindow*, QQuickItem*)> walk =
            [&](QQuickWindow* window, QQuickItem* candidate) {
                if (!candidate)
                    return;
                if (textMatches(candidate))
                    matches.append({window, candidate});
                for (QQuickItem* child : candidate->childItems())
                    walk(window, child);
            };
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            if (auto* quick = qobject_cast<QQuickWindow*>(window))
                walk(quick, quick->contentItem());
        }
        auto interactive = [](QQuickItem* item) {
            return (item->acceptedMouseButtons() & Qt::LeftButton) ||
                   item->inherits("QQuickAbstractButton") ||
                   item->inherits("QQuickMouseArea");
        };
        std::stable_sort(matches.begin(), matches.end(),
                         [&](const QmlItemMatch& left, const QmlItemMatch& right) {
                             if (left.item->isVisible() != right.item->isVisible())
                                 return left.item->isVisible();
                             if (interactive(left.item) != interactive(right.item))
                                 return interactive(left.item);
                             return left.item->width() * left.item->height() >
                                    right.item->width() * right.item->height();
                         });
    }
    if (candidates)
        *candidates = qmlCandidates(matches);
    if (matches.isEmpty()) {
        if (error)
            *error = qmlId.isEmpty()
                         ? QStringLiteral("no QML item matches target")
                         : QStringLiteral("no QML item has qml_id: %1").arg(qmlId);
        return false;
    }

    QList<QmlItemMatch> selectable;
    for (const QmlItemMatch& match : matches) {
        if (includeHidden || match.item->isVisible())
            selectable.append(match);
    }
    if (selectable.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 QML items match but all are hidden; "
                                    "pass include_hidden=true")
                         .arg(matches.size());
        return false;
    }
    if (nth < 0 && qmlId.isEmpty() && selectable.size() > 1) {
        // A label and its clickable ancestor often share the same text.
        // Fold descendants into the ancestor before choosing a target.
        QList<QmlItemMatch> folded;
        for (const QmlItemMatch& match : selectable) {
            bool hasAncestorMatch = false;
            for (const QmlItemMatch& other : selectable) {
                if (other.item != match.item &&
                    qmlContainsItem(other.item, match.item)) {
                    hasAncestorMatch = true;
                    break;
                }
            }
            if (!hasAncestorMatch)
                folded.append(match);
        }
        QList<QmlItemMatch> actionable;
        for (const QmlItemMatch& match : folded) {
            if (qmlActionable(match.item))
                actionable.append(match);
        }
        if (actionable.isEmpty()) {
            QSet<QQuickItem*> seenAncestors;
            for (const QmlItemMatch& match : folded) {
                QQuickItem* ancestor = qmlActionableAncestor(match.item);
                if (!ancestor || seenAncestors.contains(ancestor))
                    continue;
                seenAncestors.insert(ancestor);
                actionable.append({match.window, ancestor});
            }
        }
        if (actionable.size() == 1)
            selectable = actionable;
        else
            selectable = folded;
        matches = selectable;
    } else {
        matches = selectable;
    }
    if (candidates)
        *candidates = qmlCandidates(matches);
    if (matches.size() > 1 && nth < 0) {
        if (error)
            *error = QStringLiteral("%1 QML items match%2; provide nth")
                         .arg(matches.size())
                         .arg(qmlId.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" qml_id %1").arg(qmlId));
        return false;
    }
    if (nth >= matches.size()) {
        if (error)
            *error = QStringLiteral("nth %1 out of range (%2)")
                         .arg(nth)
                         .arg(matches.size());
        return false;
    }
    const QmlItemMatch match = matches.at(nth < 0 ? 0 : nth);
    if (window)
        *window = match.window;
    if (item)
        *item = match.item;
    return true;
}

QStringList methodSignatures(QObject* object, const QString& method)
{
    QStringList signatures;
    for (int i = 0; i < object->metaObject()->methodCount(); ++i) {
        const QMetaMethod candidate = object->metaObject()->method(i);
        if (candidate.methodType() == QMetaMethod::Signal)
            continue;
        if (QString::fromLatin1(candidate.name()) != method)
            continue;
        signatures.append(
            QString::fromLatin1(candidate.methodSignature()));
    }
    signatures.sort();
    return signatures;
}

QString setterPropertyName(QObject* object, const QString& method)
{
    if (!method.startsWith(QStringLiteral("set")) || method.size() <= 3)
        return {};
    QString property = method.mid(3);
    property[0] = property[0].toLower();
    const int index =
        object->metaObject()->indexOfProperty(property.toUtf8().constData());
    if (index < 0)
        return {};
    return object->metaObject()->property(index).isWritable() ? property
                                                              : QString();
}

int requestNth(const QJsonObject& params)
{
    int nth = params.value(QStringLiteral("nth")).toInt(-1);
    if (nth < 0) {
        const QJsonValue target = params.value(QStringLiteral("target"));
        if (target.isObject())
            nth = target.toObject().value(QStringLiteral("nth")).toInt(-1);
    }
    return nth;
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

// "inherits" matches any class name in the metaobject chain, so a filter on
// QAbstractButton also reaches QPushButton.
QStringList classNameChain(const QObject* object)
{
    QStringList names;
    if (!object)
        return names;
    for (const QMetaObject* meta = object->metaObject(); meta;
         meta = meta->superClass()) {
        const QString name = QString::fromLatin1(meta->className());
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names;
}

bool matchInherits(const QJsonValue& spec, const QObject* object)
{
    if (!object || !validMatcher(spec))
        return false;
    const QStringList chain = classNameChain(object);
    for (const QString& name : chain) {
        if (matchText(spec, name))
            return true;
    }
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
                             QStringLiteral("source"), QStringLiteral("windows"),
                             QStringLiteral("task")});
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
    bool truncated = false;
};

// Repeated source lookups are common while an agent inspects several controls
// from the same QML file. Keep successful lookups for this process lifetime.
QHash<QString, UiHit> qmlSourceCache;

// A missed lookup is expensive to retry; remember it per origin/roots/name.
// An entry whose file is empty is a negative result.
QHash<QString, UiHit> sourceMissCache;

constexpr int kSourceCacheLimit = 512;

// Source scans run on the GUI thread, so every walk is bounded: a fixed depth,
// a fixed number of entries and a short wall-clock budget.
struct ScanBudget {
    QElapsedTimer clock;
    int visited = 0;
    bool truncated = false;
    int maxDepth = 8;
    int maxVisited = 20000;
    int budgetMs = 120;

    ScanBudget() { clock.start(); }

    bool exhausted()
    {
        if (truncated)
            return true;
        if (visited >= maxVisited || clock.elapsed() >= budgetMs) {
            truncated = true;
            return true;
        }
        return false;
    }
};

bool skipSourceDir(const QString& name)
{
    static const QSet<QString> skipped{
        QStringLiteral(".git"), QStringLiteral(".venv"),
        QStringLiteral("venv"), QStringLiteral("node_modules"),
        QStringLiteral("CMakeFiles"), QStringLiteral("__pycache__"),
        QStringLiteral(".cache"), QStringLiteral(".tox"),
        QStringLiteral(".pytest_cache")};
    return skipped.contains(name);
}

QStringList walkFiles(const QStringList& roots, const QStringList& nameFilters,
                      ScanBudget* budget)
{
    ScanBudget localBudget;
    if (!budget)
        budget = &localBudget;
    QStringList files;
    QSet<QString> seen;
    std::function<void(const QDir&, int)> walk = [&](const QDir& dir,
                                                     int depth) {
        if (budget->exhausted())
            return;
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            if (budget->exhausted())
                return;
            ++budget->visited;
            if (entry.isDir()) {
                if (depth >= budget->maxDepth ||
                    skipSourceDir(entry.fileName()))
                    continue;
                // A CMake cache marks a build tree; its copies are generated.
                if (QFileInfo(entry.absoluteFilePath() +
                              QStringLiteral("/CMakeCache.txt"))
                        .isFile())
                    continue;
                walk(QDir(entry.absoluteFilePath()), depth + 1);
                continue;
            }
            if (!entry.isFile() ||
                !QDir::match(nameFilters, entry.fileName()))
                continue;
            // absoluteFilePath() de-duplicates nested roots; canonical paths
            // would be far too slow to resolve for every file on Windows.
            const QString path = entry.absoluteFilePath();
            if (seen.contains(path))
                continue;
            seen.insert(path);
            files.append(path);
        }
    };
    for (const QString& root : roots) {
        if (budget->exhausted())
            break;
        const QDir dir(root);
        if (!dir.exists())
            continue;
        walk(dir, 0);
    }
    return files;
}

QString sourceCacheKey(const QString& origin, const QStringList& roots,
                       const QString& name)
{
    return origin + QChar(0x1f) + roots.join(QChar(0x1f)) +
           QChar(0x1f) + name;
}

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

QStringList sourceRoots(const QString& overrideDir = QString())
{
    // An explicit source_dir is exclusive: never widen it to other candidates.
    if (!overrideDir.isEmpty()) {
        return {QDir::cleanPath(QFileInfo(overrideDir).absoluteFilePath())};
    }
    QStringList roots;
    const QByteArray configured = qgetenv("QT_COMMANDER_SOURCE_DIR");
    if (!configured.isEmpty())
        roots.append(QString::fromLocal8Bit(configured));

    // Prefer real source subdirectories over build/staging trees. Staging
    // trees can contain thousands of generated QML files and otherwise
    // exhaust the bounded scan before the source is reached.
    QStringList ancestors;
    QDir ancestor(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) {
        if (!ancestor.cdUp())
            break;
        const QString path = ancestor.absolutePath();
        if (path.size() <= 4) // "D:/" or "D:\\"
            break;
        ancestors.append(path);
        const QString sourceSubdir = path + QStringLiteral("/src");
        if (QDir(sourceSubdir).exists())
            roots.append(sourceSubdir);
    }
    roots.append(QCoreApplication::applicationDirPath());
    roots.append(QDir::currentPath());
    roots.append(ancestors);
    roots.removeDuplicates();
    return roots;
}

bool findUiLocation(const QString& objectName, const QStringList& roots,
                    UiHit* hit, ScanBudget* budget)
{
    if (objectName.isEmpty())
        return false;
    const QStringList files =
        walkFiles(roots, {QStringLiteral("*.ui")}, budget);
    for (const QString& path : files) {
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
                hit->file = path;
                hit->line = int(xml.lineNumber());
                hit->truncated = budget && budget->truncated;
                return true;
            }
        }
    }
    return false;
}

bool lineDeclaresName(const QString& line, const QString& name)
{
    const QString escaped = QRegularExpression::escape(name);
    const QRegularExpression pattern(
        QStringLiteral("\\b(?:id|objectName)\\s*:\\s*[\"']?%1[\"']?\\b")
            .arg(escaped));
    return pattern.match(line).hasMatch();
}

bool findQmlLocationInFile(const QString& path, const QString& name,
                           UiHit* hit)
{
    if (path.isEmpty() || name.isEmpty())
        return false;
    QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    int line = 0;
    while (!file.atEnd()) {
        const QString text = QString::fromUtf8(file.readLine());
        ++line;
        if (lineDeclaresName(text, name)) {
            // Resolve the canonical path only for an actual match.
            hit->file = info.canonicalFilePath();
            hit->line = line;
            return true;
        }
    }
    return false;
}

bool findQmlLocationByFileName(const QString& fileName, const QString& name,
                               const QStringList& roots, UiHit* hit,
                               ScanBudget* budget)
{
    if (fileName.isEmpty() || name.isEmpty())
        return false;
    const QStringList files = walkFiles(roots, {fileName}, budget);
    for (const QString& path : files) {
        if (findQmlLocationInFile(path, name, hit))
            return true;
    }
    return false;
}

bool findQmlLocation(const QString& objectName, const QStringList& roots,
                     UiHit* hit, ScanBudget* budget)
{
    if (objectName.isEmpty())
        return false;
    // Scope the success cache by roots so an explicit source_dir stays
    // exclusive instead of reusing results from a wider scan.
    const QString cacheKey =
        roots.join(QChar(0x1f)) + QChar(0x1f) + objectName;
    const auto cached = qmlSourceCache.constFind(cacheKey);
    if (cached != qmlSourceCache.constEnd()) {
        *hit = cached.value();
        return true;
    }
    const QStringList files =
        walkFiles(roots, {QStringLiteral("*.qml")}, budget);
    for (const QString& path : files) {
        if (findQmlLocationInFile(path, objectName, hit)) {
            hit->truncated = budget && budget->truncated;
            qmlSourceCache.insert(cacheKey, *hit);
            return true;
        }
    }
    return false;
}

// Read the lines around a hit. Bounded by the line window and an 8 KiB cap so
// the response stays inside the payload budget.
QJsonObject snippetFor(const UiHit& hit, int snippetLines, int maxLines)
{
    if (hit.file.isEmpty() || hit.line <= 0)
        return {};
    QFile file(hit.file);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const int context = qBound(0, snippetLines, 50);
    const int start = qMax(1, hit.line - context);
    const int window = qMax(1, maxLines);
    const int end =
        qMax(hit.line, qMin(hit.line + context, start + window - 1));
    QStringList lines;
    qint64 bytes = 0;
    bool clipped = false;
    int number = 0;
    while (!file.atEnd() && number < end) {
        const QByteArray raw = file.readLine();
        ++number;
        if (number < start)
            continue;
        if (bytes + raw.size() > 8 * 1024) {
            clipped = true;
            break;
        }
        bytes += raw.size();
        QString text = QString::fromUtf8(raw);
        while (text.endsWith(QLatin1Char('\n')) ||
               text.endsWith(QLatin1Char('\r')))
            text.chop(1);
        lines.append(text);
    }
    if (lines.isEmpty() && !clipped)
        return {};
    QJsonObject snippet;
    snippet.insert(QStringLiteral("startLine"), start);
    snippet.insert(QStringLiteral("lines"), QJsonArray::fromStringList(lines));
    if (clipped)
        snippet.insert(QStringLiteral("truncated"), true);
    return snippet;
}

// Find the line that declares a QML signal handler ("on<Handler>" or
// "handler:") within the configured line range of the matched location.
int findHandlerLine(const QString& file, int aroundLine, int range,
                    const QString& handler)
{
    if (file.isEmpty() || handler.isEmpty() || aroundLine <= 0)
        return 0;
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly))
        return 0;
    QString name = handler;
    if (name.size() > 2 && name.startsWith(QStringLiteral("on")) &&
        name.at(2).isUpper())
        name = name.mid(2);
    const QRegularExpression pattern(
        QStringLiteral("\\bon%1\\b|handler\\s*:")
            .arg(QRegularExpression::escape(name)),
        QRegularExpression::CaseInsensitiveOption);
    const int span = qMax(1, range);
    const int from = qMax(1, aroundLine - span);
    const int to = aroundLine + span;
    int number = 0;
    while (!source.atEnd()) {
        const QString text = QString::fromUtf8(source.readLine());
        ++number;
        if (number < from)
            continue;
        if (number > to)
            break;
        if (pattern.match(text).hasMatch())
            return number;
    }
    return 0;
}

QJsonObject searchInfo(const QStringList& roots, const ScanBudget& budget)
{
    QJsonObject search;
    search.insert(QStringLiteral("roots"), QJsonArray::fromStringList(roots));
    search.insert(QStringLiteral("visited"), budget.visited);
    search.insert(QStringLiteral("truncated"), budget.truncated);
    search.insert(QStringLiteral("budget_ms"), budget.budgetMs);
    return search;
}

// Bounded event pump used after dispatching input. It keeps the GUI thread
// responsive instead of sleeping, but never exceeds the settle budget.
int settlePump(int budgetMs)
{
    if (budgetMs <= 0)
        return 0;
    QElapsedTimer clock;
    clock.start();
    // QAbstractEventDispatcher::processEvents() returns whether it handled
    // anything in both Qt 5 and Qt 6; QCoreApplication::processEvents() became
    // void in Qt 6, and hasPendingEvents() was removed there.
    QAbstractEventDispatcher* dispatcher = QCoreApplication::eventDispatcher();
    while (clock.elapsed() < budgetMs) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool hadEvents = dispatcher
            && dispatcher->processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!hadEvents)
            break;
    }
    return int(clock.elapsed());
}

QJsonObject propertySnapshot(const QObject* object)
{
    QJsonObject props;
    if (!object)
        return props;
    static const char* const names[] = {"text", "visible", "enabled",
                                        "checked"};
    const QMetaObject* meta = object->metaObject();
    for (const char* name : names) {
        const int index = meta->indexOfProperty(name);
        if (index < 0)
            continue;
        const QMetaProperty property = meta->property(index);
        if (!property.isReadable())
            continue;
        const QVariant value = property.read(object);
        if (value.isValid())
            props.insert(QString::fromLatin1(name),
                         QJsonValue::fromVariant(value));
    }
    return props;
}

QJsonObject changedProperties(const QJsonObject& before,
                              const QJsonObject& after)
{
    QJsonObject changed;
    for (auto it = after.begin(); it != after.end(); ++it) {
        const QJsonValue previous = before.value(it.key());
        if (previous == it.value())
            continue;
        QJsonObject delta;
        delta.insert(QStringLiteral("old"), previous);
        delta.insert(QStringLiteral("new"), it.value());
        changed.insert(it.key(), delta);
    }
    return changed;
}

void addObservedChanges(QJsonObject* observed, const QObject* object,
                        const QJsonObject& before)
{
    if (!observed)
        return;
    const QJsonObject after = propertySnapshot(object);
    observed->insert(QStringLiteral("props"), after);
    observed->insert(QStringLiteral("changed"),
                     changedProperties(before, after));
}

} // namespace

Server::Server(QString token, QString tag, QObject* parent)
    : QObject(parent), token_(std::move(token)), tag_(std::move(tag))
{
    connect(&server_, &QTcpServer::newConnection,
            this, &Server::onNewConnection);
    auto* prune = new QTimer(this);
    prune->setInterval(30000);
    connect(prune, &QTimer::timeout, this, &Server::pruneTasks);
    prune->start();
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
    for (auto it = queue_.begin(); it != queue_.end();) {
        if ((*it)->socket == socket) {
            delete *it;
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
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
                         const QString& code, const QString& message,
                         const QJsonObject& details)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    error.insert(QStringLiteral("retryable"), false);
    if (!details.isEmpty())
        error.insert(QStringLiteral("details"), details);

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
        result.insert(QStringLiteral("queued"), queue_.size());
        result.insert(QStringLiteral("inflight"), inflight_.size());
        result.insert(QStringLiteral("active"), active_ != nullptr);
        sendResult(socket, id, result);
        return;
    }
    if (method == QStringLiteral("shutdown")) {
        sendResult(socket, id, QJsonObject{});
        socket->disconnectFromHost();
        return;
    }
    if (method == QStringLiteral("task")) {
        QJsonObject result = taskInfo(params);
        if (result.contains(QStringLiteral("error"))) {
            sendOperationError(socket, id, result);
        } else {
            sendResult(socket, id, result);
        }
        return;
    }
    if (method == QStringLiteral("windows") ||
        method == QStringLiteral("query") ||
        method == QStringLiteral("act") ||
        method == QStringLiteral("screenshot") ||
        method == QStringLiteral("get") ||
        method == QStringLiteral("set") ||
        method == QStringLiteral("call") ||
        method == QStringLiteral("methods") ||
        method == QStringLiteral("items") ||
        method == QStringLiteral("source")) {
        enqueue(socket, id, method, params);
        return;
    }
    sendError(socket, id, QStringLiteral("E_PROTOCOL"),
              QStringLiteral("unknown method: %1").arg(method));
}

void Server::sendOperationError(QTcpSocket* socket, const QJsonValue& id,
                                const QJsonObject& result)
{
    QJsonObject details;
    if (result.contains(QStringLiteral("candidates")))
        details.insert(QStringLiteral("candidates"),
                       result.value(QStringLiteral("candidates")));
    sendError(socket, id, result.value(QStringLiteral("error")).toString(),
              result.value(QStringLiteral("message")).toString(), details);
}

Server::~Server()
{
    // Requests that never started are safe to drop. Tasks still in inflight_
    // are sitting on a stack frame inside the target and must not be deleted.
    for (Task* task : queue_)
        delete task;
    queue_.clear();
}

void Server::enqueue(QTcpSocket* socket, const QJsonValue& id,
                     const QString& method, const QJsonObject& params)
{
    constexpr int kMaxQueued = 256;
    if (queue_.size() >= kMaxQueued) {
        sendError(socket, id, QStringLiteral("E_BUSY"),
                  QStringLiteral("too many queued requests; retry after the "
                                 "current task completes"));
        return;
    }
    auto* task = new Task;
    task->id = tag_ + QLatin1Char('-') + QString::number(++taskCounter_);
    task->method = method;
    task->params = params;
    task->requestId = id;
    task->socket = socket;
    task->mutating = method == QStringLiteral("act") ||
                     method == QStringLiteral("call") ||
                     method == QStringLiteral("set");
    if (params.contains(QStringLiteral("wait_ms")))
        task->waitMs = params.value(QStringLiteral("wait_ms")).toInt(-1);
    else if (task->mutating)
        task->waitMs =
            method == QStringLiteral("act") ? 1000 : 3000;
    task->clock.start();
    queue_.enqueue(task);
    pump();
}

void Server::pump()
{
    if (active_ || queue_.isEmpty())
        return;
    // Never start the next task from inside a response callback: post it so
    // nested event loops (modal dialogs) pick it up on their own turn.
    QMetaObject::invokeMethod(
        this, [this]() { runNext(); }, Qt::QueuedConnection);
}

void Server::runNext()
{
    if (active_ || queue_.isEmpty())
        return;
    Task* task = queue_.dequeue();
    if (task->socket.isNull()) {
        delete task;
        pump();
        return;
    }
    active_ = task;
    if (task->waitMs >= 0) {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
        task->timer = timer;
        connect(timer, &QTimer::timeout, this,
                [this, task]() { onTaskTimeout(task); });
        timer->start(task->waitMs);
    }
    const QString method = task->method;
    QJsonObject result;
    QByteArray binary;
    if (method == QStringLiteral("windows")) {
        result = windows(task->params);
    } else if (method == QStringLiteral("query")) {
        result = query(task->params);
    } else if (method == QStringLiteral("act")) {
        result = act(task->params);
    } else if (method == QStringLiteral("screenshot")) {
        result = screenshot(task->params, &binary);
    } else if (method == QStringLiteral("get")) {
        result = get(task->params);
    } else if (method == QStringLiteral("set")) {
        result = setProperty(task->params);
    } else if (method == QStringLiteral("call")) {
        result = call(task->params);
    } else if (method == QStringLiteral("methods")) {
        result = methods(task->params);
    } else if (method == QStringLiteral("items")) {
        result = items(task->params);
    } else if (method == QStringLiteral("source")) {
        result = source(task->params);
    } else {
        result.insert(QStringLiteral("error"), QStringLiteral("E_PROTOCOL"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("unknown queued method"));
    }
    finishTask(task, result, binary);
}

void Server::onTaskTimeout(Task* task)
{
    if (!task || task->responded || active_ != task)
        return;
    task->responded = true;
    if (task->timer) {
        task->timer->stop();
        task->timer->deleteLater();
        task->timer = nullptr;
    }
    if (!task->socket.isNull()) {
        QJsonObject pending;
        pending.insert(QStringLiteral("ok"), true);
        pending.insert(QStringLiteral("state"), QStringLiteral("running"));
        pending.insert(QStringLiteral("pending"), true);
        pending.insert(QStringLiteral("task_id"), task->id);
        pending.insert(QStringLiteral("method"), task->method);
        pending.insert(QStringLiteral("elapsed_ms"),
                       int(task->clock.elapsed()));
        sendResult(task->socket, task->requestId, pending);
    }
    // The task is still on the stack; keep it alive until it finishes, but
    // let the queue advance now.
    inflight_.insert(task->id, task);
    active_ = nullptr;
    pump();
}

void Server::finishTask(Task* task, const QJsonObject& result,
                        const QByteArray& binary)
{
    if (!task)
        return;
    if (task->timer) {
        task->timer->stop();
        task->timer->deleteLater();
        task->timer = nullptr;
    }
    if (task->responded) {
        CompletedTask done;
        done.result = result;
        done.binary = binary;
        done.finishedAt.start();
        completed_.insert(task->id, done);
    } else if (!task->socket.isNull()) {
        if (result.contains(QStringLiteral("error"))) {
            sendOperationError(task->socket, task->requestId, result);
        } else if (!binary.isEmpty()) {
            sendBinaryResult(task->socket, task->requestId, result, binary);
        } else {
            sendResult(task->socket, task->requestId, result);
        }
    }
    inflight_.remove(task->id);
    if (active_ == task)
        active_ = nullptr;
    delete task;
    pump();
}

QJsonObject Server::taskInfo(const QJsonObject& params)
{
    const QString id = params.value(QStringLiteral("task_id")).toString();
    QJsonObject result;
    if (id.isEmpty()) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("task_id is required"));
        return result;
    }
    if (Task* running = inflight_.value(id, nullptr)) {
        result.insert(QStringLiteral("state"), QStringLiteral("running"));
        result.insert(QStringLiteral("task_id"), id);
        result.insert(QStringLiteral("pending"), true);
        result.insert(QStringLiteral("method"), running->method);
        result.insert(QStringLiteral("elapsed_ms"),
                      int(running->clock.elapsed()));
        return result;
    }
    auto it = completed_.constFind(id);
    if (it != completed_.constEnd()) {
        result.insert(QStringLiteral("state"), QStringLiteral("done"));
        result.insert(QStringLiteral("task_id"), id);
        result.insert(QStringLiteral("result"), it->result);
        if (!it->binary.isEmpty())
            result.insert(QStringLiteral("binary_bytes"), it->binary.size());
        return result;
    }
    result.insert(QStringLiteral("error"), QStringLiteral("E_TASK_NOT_FOUND"));
    result.insert(QStringLiteral("message"),
                  QStringLiteral("unknown or expired task: %1").arg(id));
    return result;
}

void Server::pruneTasks()
{
    constexpr qint64 kTtlMs = 5 * 60 * 1000;
    for (auto it = completed_.begin(); it != completed_.end();) {
        if (it->finishedAt.isValid() && it->finishedAt.elapsed() > kTtlMs)
            it = completed_.erase(it);
        else
            ++it;
    }
    constexpr int kMaxCompleted = 256;
    while (completed_.size() > kMaxCompleted) {
        QString oldest;
        qint64 oldestAge = -1;
        for (auto it = completed_.constBegin(); it != completed_.constEnd();
             ++it) {
            const qint64 age =
                it->finishedAt.isValid() ? it->finishedAt.elapsed() : 0;
            if (age > oldestAge) {
                oldestAge = age;
                oldest = it.key();
            }
        }
        if (oldest.isEmpty())
            break;
        completed_.remove(oldest);
    }
}

QJsonObject Server::windows(const QJsonObject& params)
{
    const bool includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    const QJsonValue classSpec = params.value(QStringLiteral("class"));
    if (!classSpec.isNull() && !classSpec.isUndefined() &&
        !validMatcher(classSpec)) {
        QJsonObject error;
        error.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_FILTER"));
        error.insert(QStringLiteral("message"),
                     QStringLiteral("invalid window class matcher"));
        return error;
    }
    const bool filterVisible = params.contains(QStringLiteral("visible"));
    const bool wantedVisible =
        params.value(QStringLiteral("visible")).toBool(true);
    const int offset = qMax(0, params.value(QStringLiteral("offset")).toInt(0));
    const int limit = qMax(1, params.value(QStringLiteral("limit")).toInt(200));
    QList<QWidget*> roots = QApplication::topLevelWidgets();
    std::sort(roots.begin(), roots.end(), [](QWidget* left, QWidget* right) {
        return left->winId() < right->winId();
    });

    QJsonArray all;
    for (QWidget* root : roots) {
        if (!root || !root->isWindow())
            continue;
        if (!includeHidden && !root->isVisible())
            continue;
        if (filterVisible && root->isVisible() != wantedVisible)
            continue;
        if (!classSpec.isNull() && !classSpec.isUndefined() &&
            !matchText(classSpec, QString::fromLatin1(
                                      root->metaObject()->className())))
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
        all.append(window);
    }

    {
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            auto* quick = qobject_cast<QQuickWindow*>(window);
            if (!quick)
                continue;
            if (!includeHidden && !quick->isVisible())
                continue;
            if (filterVisible && quick->isVisible() != wantedVisible)
                continue;
            if (!classSpec.isNull() && !classSpec.isUndefined() &&
                !matchText(classSpec, QString::fromLatin1(
                                          quick->metaObject()->className())))
                continue;
            const QPoint global = quick->position();
            QJsonObject screenPos;
            screenPos.insert(QStringLiteral("x"), global.x());
            screenPos.insert(QStringLiteral("y"), global.y());
            QJsonObject size;
            size.insert(QStringLiteral("w"), quick->width());
            size.insert(QStringLiteral("h"), quick->height());
            QJsonObject windowObject;
            windowObject.insert(QStringLiteral("id"), idForWindow(quick));
            windowObject.insert(QStringLiteral("class"), QStringLiteral("QQuickWindow"));
            windowObject.insert(QStringLiteral("objectName"), quick->objectName());
            windowObject.insert(QStringLiteral("title"), quick->title());
            windowObject.insert(QStringLiteral("rect"), size);
            windowObject.insert(QStringLiteral("screen_pos"), screenPos);
            windowObject.insert(QStringLiteral("visible"), quick->isVisible());
            windowObject.insert(QStringLiteral("minimized"),
                               quick->windowStates().testFlag(Qt::WindowMinimized));
            windowObject.insert(QStringLiteral("active"), quick->isActive());
            windowObject.insert(QStringLiteral("modal"), quick->modality() != Qt::NonModal);
            windowObject.insert(QStringLiteral("popup"),
                               quick->flags().testFlag(Qt::Popup));
            windowObject.insert(QStringLiteral("tooltip"),
                               quick->flags().testFlag(Qt::ToolTip));
            if (QScreen* screen = quick->screen())
                windowObject.insert(QStringLiteral("screen"), screen->name());
            windowObject.insert(
                QStringLiteral("child_count"),
                quick->contentItem() ? quick->contentItem()->childItems().size()
                                      : 0);
            all.append(windowObject);
        }
    }

    QJsonArray page;
    for (int i = offset; i < all.size() && i < offset + limit; ++i)
        page.append(all.at(i));
    QJsonObject result;
    result.insert(QStringLiteral("windows"), page);
    result.insert(QStringLiteral("count"), page.size());
    result.insert(QStringLiteral("total"), all.size());
    result.insert(QStringLiteral("offset"), offset);
    result.insert(QStringLiteral("limit"), limit);
    result.insert(QStringLiteral("truncated"), all.size() > offset + page.size());
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
        QStringLiteral("text"), QStringLiteral("props"),
        QStringLiteral("inherits")};
    for (auto it = context.where.begin(); it != context.where.end(); ++it) {
        if (!allowedWhere.contains(it.key())) {
            QJsonObject error;
            error.insert(QStringLiteral("error"),
                         QStringLiteral("E_INVALID_FILTER"));
            error.insert(
                QStringLiteral("message"),
                QStringLiteral(
                    "unsupported locator key: %1 (supported: id, class, "
                    "object_name/objectName, qml_id, qml_type, text, props, "
                    "inherits)")
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
    if (context.props.isUndefined() || context.props.isNull()) {
        // Default to the properties an agent needs to identify a node.
        // Callers can still pass [] for a minimal tree or "*" for everything.
        context.props =
            QJsonArray{QStringLiteral("text"), QStringLiteral("visible"),
                       QStringLiteral("enabled")};
    }
    context.includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    context.maxDepth = params.value(QStringLiteral("max_depth")).toInt(-1);
    context.compact = params.value(QStringLiteral("compact")).toBool(false);
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
    QWindow* scopeWindow = nullptr;
    QQuickItem* scopeItem = nullptr;
    QQuickWindow* scopeItemWindow = nullptr;
    if (params.contains(QStringLiteral("scope"))) {
        const QJsonValue scope = params.value(QStringLiteral("scope"));
        if (!scope.isNull() && !scope.isUndefined()) {
            const QString scopeId = scope.isString()
                                        ? scope.toString()
                                        : scope.toObject()
                                              .value(QStringLiteral("id"))
                                              .toString();
            const QString scopeQmlId =
                scope.isObject()
                    ? scope.toObject().value(QStringLiteral("qml_id")).toString()
                    : QString();
            if (virtuals_.contains(scopeId)) {
                const VirtualRef ref = virtuals_.value(scopeId);
                if (ref.kind == QStringLiteral("qml_item")) {
                    scopeItem =
                        qobject_cast<QQuickItem*>(ref.qmlObject.data());
                    scopeItemWindow =
                        qobject_cast<QQuickWindow*>(ref.windowOwner.data());
                }
            }
            if (!scopeItem && !scopeQmlId.isEmpty()) {
                const QList<QmlItemMatch> matches =
                    findQmlMatches(scopeQmlId);
                const QJsonObject scopeObject = scope.toObject();
                const int requestedNth =
                    scopeObject.contains(QStringLiteral("nth"))
                        ? scopeObject.value(QStringLiteral("nth")).toInt(-1)
                        : -1;
                if (matches.size() > 1 && requestedNth < 0) {
                    QJsonObject error;
                    error.insert(QStringLiteral("error"),
                                 QStringLiteral("E_AMBIGUOUS"));
                    error.insert(QStringLiteral("message"),
                                 QStringLiteral("%1 QML items have qml_id %2; "
                                                "provide scope.nth")
                                     .arg(matches.size())
                                     .arg(scopeQmlId));
                    QJsonArray candidates;
                    for (int i = 0; i < matches.size(); ++i) {
                        const QmlItemMatch& match = matches.at(i);
                        const QRect rect =
                            match.item->mapRectToScene(match.item->boundingRect())
                                .toAlignedRect();
                        QJsonObject candidate;
                        candidate.insert(QStringLiteral("nth"), i);
                        candidate.insert(QStringLiteral("visible"),
                                         match.item->isVisible());
                        candidate.insert(QStringLiteral("class"),
                                         QString::fromLatin1(
                                             match.item->metaObject()->className()));
                        candidate.insert(QStringLiteral("rect"), QJsonObject{
                            {QStringLiteral("x"), rect.x()},
                            {QStringLiteral("y"), rect.y()},
                            {QStringLiteral("w"), rect.width()},
                            {QStringLiteral("h"), rect.height()}});
                        candidates.append(candidate);
                    }
                    error.insert(QStringLiteral("candidates"), candidates);
                    return error;
                }
                if (requestedNth >= matches.size()) {
                    QJsonObject error;
                    error.insert(QStringLiteral("error"),
                                 QStringLiteral("E_INVALID_ARGUMENT"));
                    error.insert(QStringLiteral("message"),
                                 QStringLiteral("scope.nth %1 out of range (%2)")
                                     .arg(requestedNth)
                                     .arg(matches.size()));
                    return error;
                }
                if (!matches.isEmpty()) {
                    const QmlItemMatch match =
                        matches.at(requestedNth < 0 ? 0 : requestedNth);
                    scopeItem = match.item;
                    scopeItemWindow = match.window;
                }
            }
            scopeWindow =
                scopeItem ? nullptr : windowForId(scopeId);
            if (!scopeWindow && !scopeItem) {
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
    QList<QQuickWindow*> quickRoots;
    if (scopeItem) {
        // The QML item itself is the query root.
    } else if (scopeWindow) {
        if (auto* quick = qobject_cast<QQuickWindow*>(scopeWindow))
            quickRoots.append(quick);
    } else if (!scopeRoot) {
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            if (auto* quick = qobject_cast<QQuickWindow*>(window)) {
                if (context.includeHidden || quick->isVisible())
                    quickRoots.append(quick);
            }
        }
    }

    if (countOnly) {
        for (QWidget* root : roots) {
            if (!root)
                continue;
            if (!isCandidate(root, context.includeHidden)) {
                ++context.hiddenPruned;
                continue;
            }
            countNode(root, context, 0);
        }
        for (QQuickWindow* root : quickRoots) {
            bool any = false;
            buildQuickWindowNode(root, context, true, &any);
        }
        if (scopeItem && scopeItemWindow) {
            bool any = false;
            int serial = 0;
            buildQuickItemNode(scopeItemWindow, scopeItem, context, 0, &serial,
                               &any);
        }
        QJsonObject result;
        result.insert(QStringLiteral("count"),
                      context.hasWhere ? context.matched : context.total);
        if (!context.includeHidden && context.hiddenPruned > 0)
            result.insert(QStringLiteral("hidden_pruned"),
                          context.hiddenPruned);
        return result;
    }

    QJsonArray tree;
    for (QWidget* root : roots) {
        if (!root)
            continue;
        if (!isCandidate(root, context.includeHidden)) {
            ++context.hiddenPruned;
            continue;
        }
        bool any = false;
        const QJsonObject node = buildNode(root, context, 0, true, &any);
        if (any)
            tree.append(node);
    }
    for (QQuickWindow* root : quickRoots) {
        bool any = false;
        const QJsonObject node = buildQuickWindowNode(root, context, true, &any);
        if (any)
            tree.append(node);
    }
    if (scopeItem && scopeItemWindow) {
        bool any = false;
        int serial = 0;
        const QJsonObject node =
            buildQuickItemNode(scopeItemWindow, scopeItem, context, 0, &serial,
                               &any);
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
    const bool truncated = limit > 0 && count > limit;

    QJsonObject result;
    result.insert(QStringLiteral("tree"), tree);
    result.insert(QStringLiteral("count"), count);
    result.insert(QStringLiteral("total"), count);
    result.insert(QStringLiteral("limit"), limit);
    result.insert(QStringLiteral("truncated"), truncated);
    result.insert(QStringLiteral("virtual_count"), virtuals_.size());
    if (!context.includeHidden && context.hiddenPruned > 0)
        result.insert(QStringLiteral("hidden_pruned"), context.hiddenPruned);
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

    if (!virtualId.isEmpty()) {
        if (QWindow* window = windowForId(virtualId)) {
            const QString windowAction =
                params.value(QStringLiteral("action")).toString();
            if (windowAction == QStringLiteral("set_geometry")) {
                const QJsonObject geometry =
                    params.value(QStringLiteral("geometry")).toObject();
                const int x = geometry.value(QStringLiteral("x")).toInt(0);
                const int y = geometry.value(QStringLiteral("y")).toInt(0);
                const int w = geometry.value(QStringLiteral("w")).toInt(0);
                const int h = geometry.value(QStringLiteral("h")).toInt(0);
                if (w <= 0 || h <= 0) {
                    QJsonObject result;
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("E_INVALID_ARGUMENT"));
                    result.insert(QStringLiteral("message"),
                                  QStringLiteral("geometry requires positive "
                                                 "w and h"));
                    return result;
                }
                window->setGeometry(QRect(x, y, w, h));
                window->show();
                window->raise();
                window->requestActivate();
                QCoreApplication::processEvents(
                    QEventLoop::ExcludeUserInputEvents);
                const QRect actual = window->geometry();
                QJsonObject requested;
                requested.insert(QStringLiteral("x"), x);
                requested.insert(QStringLiteral("y"), y);
                requested.insert(QStringLiteral("w"), w);
                requested.insert(QStringLiteral("h"), h);
                QJsonObject rect;
                rect.insert(QStringLiteral("x"), actual.x());
                rect.insert(QStringLiteral("y"), actual.y());
                rect.insert(QStringLiteral("w"), actual.width());
                rect.insert(QStringLiteral("h"), actual.height());
                QJsonObject resolved;
                resolved.insert(QStringLiteral("id"), virtualId);
                resolved.insert(
                    QStringLiteral("class"),
                    QString::fromLatin1(window->metaObject()->className()));
                resolved.insert(QStringLiteral("visible"), window->isVisible());
                resolved.insert(QStringLiteral("rect"), rect);
                QJsonObject result;
                result.insert(QStringLiteral("ok"), true);
                result.insert(QStringLiteral("requested"), requested);
                result.insert(QStringLiteral("applied"), actual == QRect(x, y, w, h));
                result.insert(QStringLiteral("resolved"), resolved);
                return result;
            }
            if (windowAction == QStringLiteral("minimize")) {
                window->showMinimized();
                QJsonObject resolved;
                resolved.insert(QStringLiteral("id"), virtualId);
                resolved.insert(
                    QStringLiteral("class"),
                    QString::fromLatin1(window->metaObject()->className()));
                resolved.insert(QStringLiteral("minimized"),
                                window->windowState() == Qt::WindowMinimized);
                const QRect geometry = window->geometry();
                resolved.insert(QStringLiteral("rect"), QJsonObject{
                    {QStringLiteral("x"), geometry.x()},
                    {QStringLiteral("y"), geometry.y()},
                    {QStringLiteral("w"), geometry.width()},
                    {QStringLiteral("h"), geometry.height()}});
                QJsonObject result;
                result.insert(QStringLiteral("ok"), true);
                result.insert(QStringLiteral("resolved"), resolved);
                return result;
            }
            if (windowAction == QStringLiteral("restore")) {
                const QRect savedGeometry = window->geometry();
                if (window->windowState() == Qt::WindowMinimized)
                    window->setWindowState(Qt::WindowNoState);
                window->show();
                if (savedGeometry.isValid())
                    window->setGeometry(savedGeometry);
                window->raise();
                window->requestActivate();
                QJsonObject resolved;
                resolved.insert(QStringLiteral("id"), virtualId);
                resolved.insert(
                    QStringLiteral("class"),
                    QString::fromLatin1(window->metaObject()->className()));
                resolved.insert(QStringLiteral("visible"), window->isVisible());
                resolved.insert(
                    QStringLiteral("minimized"),
                    window->windowState() == Qt::WindowMinimized);
                const QRect geometry = window->geometry();
                resolved.insert(QStringLiteral("rect"), QJsonObject{
                    {QStringLiteral("x"), geometry.x()},
                    {QStringLiteral("y"), geometry.y()},
                    {QStringLiteral("w"), geometry.width()},
                    {QStringLiteral("h"), geometry.height()}});
                QJsonObject result;
                result.insert(QStringLiteral("ok"), true);
                result.insert(QStringLiteral("resolved"), resolved);
                return result;
            }
            if (windowAction == QStringLiteral("click") ||
                windowAction == QStringLiteral("dblclick")) {
                const QJsonValue positionValue =
                    params.value(QStringLiteral("position"));
                if (!positionValue.isObject()) {
                    QJsonObject result;
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("E_INVALID_ARGUMENT"));
                    result.insert(QStringLiteral("message"),
                                  QStringLiteral("window click requires "
                                                 "position={x,y}"));
                    return result;
                }
                const QJsonObject position = positionValue.toObject();
                const QPointF point(position.value(QStringLiteral("x"))
                                        .toDouble(),
                                    position.value(QStringLiteral("y"))
                                        .toDouble());
                window->requestActivate();
                QCoreApplication::processEvents(
                    QEventLoop::ExcludeUserInputEvents);
                const int clicks =
                    windowAction == QStringLiteral("dblclick") ? 2 : 1;
                for (int i = 0; i < clicks; ++i) {
                    QMouseEvent move(QEvent::MouseMove, point, point,
                                     Qt::NoButton, Qt::NoButton,
                                     Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &move);
                    QMouseEvent press(QEvent::MouseButtonPress, point, point,
                                      Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease, point,
                                        point, Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &release);
                }
                QCoreApplication::processEvents();
                const QRect geometry = window->geometry();
                QJsonObject resolved;
                resolved.insert(QStringLiteral("id"), virtualId);
                resolved.insert(
                    QStringLiteral("class"),
                    QString::fromLatin1(window->metaObject()->className()));
                resolved.insert(QStringLiteral("rect"), QJsonObject{
                    {QStringLiteral("x"), geometry.x()},
                    {QStringLiteral("y"), geometry.y()},
                    {QStringLiteral("w"), geometry.width()},
                    {QStringLiteral("h"), geometry.height()}});
                resolved.insert(QStringLiteral("point"), QJsonObject{
                    {QStringLiteral("x"), point.x()},
                    {QStringLiteral("y"), point.y()}});
                QJsonObject result;
                result.insert(QStringLiteral("ok"), true);
                result.insert(QStringLiteral("action"), windowAction);
                result.insert(QStringLiteral("dispatched"), true);
                result.insert(QStringLiteral("input"), QStringLiteral("mouse"));
                result.insert(QStringLiteral("resolved"), resolved);
                return result;
            }
            if (windowAction != QStringLiteral("key")) {
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_INVALID_ARGUMENT"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("window targets support key, click, "
                                             "dblclick, minimize, restore, and "
                                             "set_geometry"));
                return result;
            }
            QStringList names;
            const QJsonValue keysValue = params.value(QStringLiteral("keys"));
            if (keysValue.isArray()) {
                for (const QJsonValue& value : keysValue.toArray())
                    names.append(value.toString());
            } else if (!params.value(QStringLiteral("key")).toString().isEmpty()) {
                names.append(params.value(QStringLiteral("key")).toString());
            }
            for (const QString& name : names) {
                const QKeySequence sequence(name);
                if (sequence.isEmpty())
                    continue;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const int key = sequence[0].key();
#else
                const int key = sequence[0];
#endif
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &press);
                QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &release);
                QCoreApplication::processEvents();
            }
            QJsonObject resolved;
            resolved.insert(QStringLiteral("id"), virtualId);
            resolved.insert(QStringLiteral("class"),
                            QString::fromLatin1(
                                window->metaObject()->className()));
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("action"), windowAction);
            result.insert(QStringLiteral("dispatched"), true);
            result.insert(QStringLiteral("resolved"), resolved);
            result.insert(QStringLiteral("observed"), resolved);
            return result;
        }
    }

    QString error;
    const int nth = requestNth(params);
    QQuickWindow* qmlWindow = nullptr;
    QQuickItem* qmlItem = nullptr;
    QString qmlError;
    QJsonArray qmlCandidateList;
    if (resolveQmlTarget(params.value(QStringLiteral("target")), nth,
                         &qmlWindow, &qmlItem, &qmlError,
                         &qmlCandidateList,
                         params.value(QStringLiteral("include_hidden"))
                             .toBool(false))) {
        if (!qmlItem->isVisible() &&
            !params.value(QStringLiteral("include_hidden")).toBool(false)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML target is hidden; pass "
                                         "include_hidden=true to override"));
            return result;
        }
        const QRect rect =
            qmlItem->mapRectToScene(qmlItem->boundingRect()).toAlignedRect();
        const QString qmlTargetId =
            registerQuickQml(qmlWindow, qmlItem, 0, rect, QJsonObject{});
        return actVirtual(params, qmlTargetId);
    }
    if (params.value(QStringLiteral("target")).isObject() &&
        params.value(QStringLiteral("target"))
            .toObject()
            .contains(QStringLiteral("qml_id"))) {
        QJsonObject result;
        result.insert(
            QStringLiteral("error"),
            qmlError.contains(QStringLiteral("all are hidden"))
                ? QStringLiteral("E_HIDDEN")
                : (qmlError.contains(QStringLiteral("provide nth"))
                       ? QStringLiteral("E_AMBIGUOUS")
                       : QStringLiteral("E_NOT_FOUND")));
        result.insert(QStringLiteral("message"), qmlError);
        if (!qmlCandidateList.isEmpty())
            result.insert(QStringLiteral("candidates"), qmlCandidateList);
        return result;
    }
    if (params.value(QStringLiteral("target")).isObject() &&
        params.value(QStringLiteral("target"))
            .toObject()
            .contains(QStringLiteral("text"))) {
        QJsonObject result;
        result.insert(
            QStringLiteral("error"),
            qmlError.contains(QStringLiteral("all are hidden"))
                ? QStringLiteral("E_HIDDEN")
                : (qmlError.contains(QStringLiteral("provide nth"))
                       ? QStringLiteral("E_AMBIGUOUS")
                       : QStringLiteral("E_NOT_FOUND")));
        result.insert(QStringLiteral("message"), qmlError);
        if (!qmlCandidateList.isEmpty())
            result.insert(QStringLiteral("candidates"), qmlCandidateList);
        return result;
    }
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
    const QJsonObject observedBefore = propertySnapshot(widget);

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
    const int settledMs = settlePump(settleMs);

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("action"), action);
    result.insert(QStringLiteral("dispatched"), true);
    result.insert(QStringLiteral("settled_ms"), settledMs);
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
        addObservedChanges(&observed, observedWidget, observedBefore);
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

    const int nth = requestNth(params);
    const bool explicitTarget = params.contains(QStringLiteral("target"));
    QWidget* widget = nullptr;
    QRect virtualCrop;
    bool virtualTarget = false;
    QString virtualId;
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    if (explicitTarget) {
        const QString quickId = targetValue.isString()
                                    ? targetValue.toString()
                                    : targetValue.toObject()
                                          .value(QStringLiteral("id"))
                                          .toString();
        if (QWindow* quick = windowForId(quickId)) {
            if (auto* quickWindow = qobject_cast<QQuickWindow*>(quick))
                return quickScreenshot(quickWindow, params, binary);
        }
    }
    const QString requestedVirtualId =
        targetValue.isString()
            ? targetValue.toString()
            : targetValue.toObject().value(QStringLiteral("id")).toString();
    if (!requestedVirtualId.isEmpty() &&
        virtuals_.contains(requestedVirtualId)) {
        if (annotate != QStringLiteral("none")) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_NOT_IMPLEMENTED"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("annotate is not supported for virtual items"));
            return result;
        }
        virtualId = requestedVirtualId;
        const VirtualRef ref = virtuals_.value(virtualId);
        if (ref.kind == QStringLiteral("qml_item") &&
            !ref.windowOwner.isNull()) {
            auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
            auto* window =
                qobject_cast<QQuickWindow*>(ref.windowOwner.data());
            if (item && window) {
                const QRect sceneRect =
                    item->mapRectToScene(item->boundingRect()).toAlignedRect();
                return quickScreenshot(window, params, binary, sceneRect);
            }
        }
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
            QList<QQuickWindow*> quickWindows;
            for (QWindow* window : QGuiApplication::topLevelWindows()) {
                if (auto* quick = qobject_cast<QQuickWindow*>(window)) {
                    if (quick->isVisible())
                        quickWindows.append(quick);
                }
            }
            if (quickWindows.size() == 1)
                return quickScreenshot(quickWindows.first(), params, binary);
            if (quickWindows.size() > 1) {
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_AMBIGUOUS"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("multiple visible QQuickWindows; "
                                             "pass target"));
                return result;
            }
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
        const int annotateOffset =
            params.value(QStringLiteral("annotate_offset")).toInt(0);
        if (annotateOffset > 0) {
            if (annotateOffset >= candidates.size())
                candidates.clear();
            else
                candidates = candidates.mid(annotateOffset);
        }
        annotateTotal = candidates.size() + virtualAnnotations.size() +
                        annotateOffset;
        annotateTruncated =
            annotateTotal > annotateOffset + maxLabels;
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
        int number = annotateOffset;
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
        result.insert(
            QStringLiteral("annotate_offset"),
            params.value(QStringLiteral("annotate_offset")).toInt(0));
        result.insert(QStringLiteral("annotate_truncated"), annotateTruncated);
    }
    if (virtualTarget) {
        result.insert(QStringLiteral("virtual"), true);
        result.insert(QStringLiteral("target"), virtualId);
    }
    return result;
}

QJsonObject Server::quickScreenshot(QQuickWindow* window,
                                     const QJsonObject& params,
                                     QByteArray* binary, const QRect& crop)
{
    if (!window) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"), QStringLiteral("no QQuickWindow"));
        return result;
    }
    QImage image = window->grabWindow();
    if (image.isNull() && window->screen())
        image = window->screen()->grabWindow(window->winId()).toImage();
    if (image.isNull()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("nothing to capture"));
        return result;
    }
    const double dpr = image.devicePixelRatio() > 0
                           ? image.devicePixelRatio()
                           : window->devicePixelRatio();
    if (!crop.isEmpty()) {
        QRect deviceCrop(qRound(crop.x() * dpr), qRound(crop.y() * dpr),
                         qRound(crop.width() * dpr),
                         qRound(crop.height() * dpr));
        deviceCrop = deviceCrop.intersected(image.rect());
        if (deviceCrop.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML item is outside the window"));
            return result;
        }
        image = image.copy(deviceCrop);
        image.setDevicePixelRatio(dpr);
    }
    const int maxWidth = params.value(QStringLiteral("max_width")).toInt(1024);
    if (maxWidth > 0 && qRound(image.width() / dpr) > maxWidth) {
        image = image.scaledToWidth(qRound(maxWidth * dpr),
                                    Qt::SmoothTransformation);
        image.setDevicePixelRatio(dpr);
    }
    const QString annotate = params.value(QStringLiteral("annotate"))
                                 .toString(QStringLiteral("none"));
    QJsonArray legend;
    int shown = 0;
    int total = 0;
    if (annotate != QStringLiteral("none") && crop.isEmpty()) {
        const QJsonObject where =
            params.value(QStringLiteral("where")).toObject();
        const int maxLabels =
            params.value(QStringLiteral("max_labels")).toInt(50);
        const int annotateOffset =
            params.value(QStringLiteral("annotate_offset")).toInt(0);
        struct QmlCandidate {
            QQuickItem* item = nullptr;
            QRect rect;
        };
        QList<QmlCandidate> candidates;
        int serial = 0;
        auto quickMatches = [&](QQuickItem* item) {
            if (where.isEmpty())
                return true;
            const QString className =
                QString::fromLatin1(item->metaObject()->className());
            if (where.contains(QStringLiteral("class")) &&
                !matchText(where.value(QStringLiteral("class")), className))
                return false;
            if (where.contains(QStringLiteral("qml_type")) &&
                !matchText(where.value(QStringLiteral("qml_type")), className))
                return false;
            if (where.contains(QStringLiteral("qml_id"))) {
                QQmlContext* context = QQmlEngine::contextForObject(item);
                const QString qmlId =
                    context ? context->nameForObject(item) : QString();
                if (!matchText(where.value(QStringLiteral("qml_id")), qmlId))
                    return false;
            }
            const QJsonValue objectSpec =
                where.contains(QStringLiteral("object_name"))
                    ? where.value(QStringLiteral("object_name"))
                    : where.value(QStringLiteral("objectName"));
            if ((where.contains(QStringLiteral("object_name")) ||
                 where.contains(QStringLiteral("objectName"))) &&
                !matchText(objectSpec, item->objectName()))
                return false;
            if (where.contains(QStringLiteral("text")) &&
                !matchText(where.value(QStringLiteral("text")),
                           item->property("text").toString()))
                return false;
            if (where.contains(QStringLiteral("props"))) {
                const QJsonObject props =
                    where.value(QStringLiteral("props")).toObject();
                for (auto it = props.begin(); it != props.end(); ++it) {
                    const QVariant value =
                        item->property(it.key().toUtf8().constData());
                    if (!value.isValid() ||
                        QJsonValue::fromVariant(value) != it.value())
                        return false;
                }
            }
            return true;
        };
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            if (!item || !item->isVisible())
                return;
            const bool matched = quickMatches(item);
            if (matched) {
                const QRect rect =
                    item->mapRectToScene(item->boundingRect()).toAlignedRect();
                if (!rect.isEmpty())
                    candidates.append({item, rect});
            }
            ++serial;
            for (QQuickItem* child : item->childItems())
                walk(child);
        };
        walk(window->contentItem());
        if (annotate == QStringLiteral("all")) {
            QList<QmlCandidate> leaves;
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
        auto qmlPriority = [](QQuickItem* item) {
            QQmlContext* context = QQmlEngine::contextForObject(item);
            const bool named =
                context && !context->nameForObject(item).isEmpty();
            const bool interactive =
                (item->acceptedMouseButtons() & Qt::LeftButton) ||
                item->inherits("QQuickAbstractButton") ||
                item->inherits("QQuickMouseArea");
            return (named ? 0 : 2) + (interactive ? 0 : 1);
        };
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const QmlCandidate& left,
                             const QmlCandidate& right) {
                             return qmlPriority(left.item) <
                                    qmlPriority(right.item);
                         });
        total = candidates.size();
        if (annotate == QStringLiteral("interactive")) {
            QList<QmlCandidate> filtered;
            for (const QmlCandidate& candidate : candidates) {
                QQuickItem* item = candidate.item;
                if (!item->isEnabled())
                    continue;
                if ((item->acceptedMouseButtons() & Qt::LeftButton) ||
                    item->inherits("QQuickAbstractButton") ||
                    item->inherits("QQuickMouseArea"))
                    filtered.append(candidate);
            }
            candidates = filtered;
        }
        if (annotateOffset > 0) {
            if (annotateOffset >= candidates.size())
                candidates.clear();
            else
                candidates = candidates.mid(annotateOffset);
        }
        if (candidates.size() > maxLabels)
            candidates = candidates.mid(0, maxLabels);
        shown = candidates.size();
        const double scale =
            window->width() > 0 ? double(image.width()) / window->width() : 1.0;
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        QPen pen(QColor(220, 40, 40));
        pen.setWidth(qMax(1, qRound(2 * dpr)));
        painter.setPen(pen);
        QFont font = painter.font();
        font.setPixelSize(qMax(8, qRound(9 * dpr)));
        painter.setFont(font);
        int number = annotateOffset;
        for (const QmlCandidate& candidate : candidates) {
            ++number;
            const QRect drawRect(
                qRound(candidate.rect.x() * scale),
                qRound(candidate.rect.y() * scale),
                qRound(candidate.rect.width() * scale),
                qRound(candidate.rect.height() * scale));
            painter.drawRect(drawRect);
            const int badgeW = qMin(drawRect.width(), qRound(20 * dpr));
            const int badgeH = qMin(drawRect.height(), qRound(14 * dpr));
            if (badgeW > 0 && badgeH > 0) {
                const QRect badge(drawRect.topLeft(), QSize(badgeW, badgeH));
                painter.fillRect(badge, QColor(255, 255, 255, 230));
                painter.drawRect(badge);
                painter.drawText(badge, Qt::AlignCenter, QString::number(number));
            }
            QJsonObject entry;
            entry.insert(QStringLiteral("n"), number);
            entry.insert(QStringLiteral("id"),
                         registerQuickQml(window, candidate.item, serial++,
                                          candidate.rect, QJsonObject{}));
            entry.insert(QStringLiteral("class"),
                         QString::fromLatin1(
                             candidate.item->metaObject()->className()));
            entry.insert(QStringLiteral("qml_type"),
                         QString::fromLatin1(
                             candidate.item->metaObject()->className()));
            QQmlContext* qmlContext =
                QQmlEngine::contextForObject(candidate.item);
            entry.insert(QStringLiteral("qml_id"),
                         qmlContext ? qmlContext->nameForObject(candidate.item)
                                    : QString());
            entry.insert(QStringLiteral("objectName"),
                         candidate.item->objectName());
            entry.insert(QStringLiteral("text"),
                         candidate.item->property("text").toString());
            entry.insert(QStringLiteral("rect"), QJsonObject{
                {QStringLiteral("x"), candidate.rect.x()},
                {QStringLiteral("y"), candidate.rect.y()},
                {QStringLiteral("w"), candidate.rect.width()},
                {QStringLiteral("h"), candidate.rect.height()}});
            entry.insert(QStringLiteral("image_rect"), QJsonObject{
                {QStringLiteral("x"), drawRect.x()},
                {QStringLiteral("y"), drawRect.y()},
                {QStringLiteral("w"), drawRect.width()},
                {QStringLiteral("h"), drawRect.height()}});
            const QPoint screen =
                window->mapToGlobal(candidate.rect.topLeft());
            entry.insert(QStringLiteral("screen_rect"), QJsonObject{
                {QStringLiteral("x"), screen.x()},
                {QStringLiteral("y"), screen.y()},
                {QStringLiteral("w"), candidate.rect.width()},
                {QStringLiteral("h"), candidate.rect.height()}});
            legend.append(entry);
        }
        painter.end();
    }

    QBuffer buffer(binary);
    if (!buffer.open(QIODevice::WriteOnly)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"),
                      QStringLiteral("E_INJECTION_FAILED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("PNG buffer failed"));
        return result;
    }
    if (!image.save(&buffer, "PNG")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"),
                      QStringLiteral("E_INJECTION_FAILED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("PNG encode failed"));
        return result;
    }
    QJsonObject result;
    result.insert(QStringLiteral("format"), QStringLiteral("png"));
    result.insert(QStringLiteral("width"),
                  qRound(image.width() / dpr));
    result.insert(QStringLiteral("height"),
                  qRound(image.height() / dpr));
    result.insert(QStringLiteral("pixel_width"), image.width());
    result.insert(QStringLiteral("pixel_height"), image.height());
    result.insert(QStringLiteral("dpr"), dpr);
    result.insert(QStringLiteral("bytes"), binary->size());
    result.insert(QStringLiteral("annotate"), annotate);
    result.insert(QStringLiteral("annotate_shown"), shown);
    result.insert(QStringLiteral("annotate_total"), total);
    result.insert(QStringLiteral("annotate_offset"),
                  params.value(QStringLiteral("annotate_offset")).toInt(0));
    result.insert(
        QStringLiteral("annotate_truncated"),
        total > params.value(QStringLiteral("annotate_offset")).toInt(0) + shown);
    result.insert(QStringLiteral("legend"), legend);
    result.insert(QStringLiteral("window"), QJsonObject{
        {QStringLiteral("id"), idForWindow(window)},
        {QStringLiteral("title"), window->title()},
        {QStringLiteral("class"), QStringLiteral("QQuickWindow")}});
    return result;
}

QJsonObject Server::get(const QJsonObject& params)
{
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    const QString virtualId =
        targetValue.isString()
            ? targetValue.toString()
            : targetValue.toObject().value(QStringLiteral("id")).toString();
    if (!virtualId.isEmpty() && virtuals_.contains(virtualId))
        return virtualItem(virtualId,
                           params.value(QStringLiteral("props")));

    QQuickWindow* qmlWindow = nullptr;
    QQuickItem* qmlItem = nullptr;
    QString qmlError;
    QJsonArray qmlCandidateList;
    if (resolveQmlTarget(targetValue, requestNth(params),
                         &qmlWindow, &qmlItem, &qmlError,
                         &qmlCandidateList,
                         params.value(QStringLiteral("include_hidden"))
                             .toBool(false))) {
        const QRect rect =
            qmlItem->mapRectToScene(qmlItem->boundingRect()).toAlignedRect();
        const QString id =
            registerQuickQml(qmlWindow, qmlItem, 0, rect, QJsonObject{});
        return virtualItem(id, params.value(QStringLiteral("props")));
    }
    if (targetValue.isObject() &&
        targetValue.toObject().contains(QStringLiteral("qml_id"))) {
        QJsonObject result;
        result.insert(
            QStringLiteral("error"),
            qmlError.contains(QStringLiteral("all are hidden"))
                ? QStringLiteral("E_HIDDEN")
                : (qmlError.contains(QStringLiteral("provide nth"))
                       ? QStringLiteral("E_AMBIGUOUS")
                       : QStringLiteral("E_NOT_FOUND")));
        result.insert(QStringLiteral("message"), qmlError);
        if (!qmlCandidateList.isEmpty())
            result.insert(QStringLiteral("candidates"), qmlCandidateList);
        return result;
    }

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        requestNth(params),
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
    const QJsonValue targetValue = params.value(QStringLiteral("target"));
    const QString virtualId =
        targetValue.isString()
            ? targetValue.toString()
            : targetValue.toObject().value(QStringLiteral("id")).toString();
    if (!virtualId.isEmpty() && virtuals_.contains(virtualId)) {
        const VirtualRef ref = virtuals_.value(virtualId);
        auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        if (ref.kind == QStringLiteral("qml_item") && item) {
            if (!item->isVisible() &&
                !params.value(QStringLiteral("include_hidden")).toBool(false)) {
                QJsonObject result;
                result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("QML target is hidden; pass "
                                             "include_hidden=true to override"));
                return result;
            }
            const QString name =
                params.value(QStringLiteral("name")).toString();
            const int index =
                item->metaObject()->indexOfProperty(name.toUtf8().constData());
            if (index < 0) {
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_INVALID_ARGUMENT"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("no such QML property: %1").arg(name));
                return result;
            }
            const QMetaProperty property = item->metaObject()->property(index);
            if (!property.isWritable()) {
                QJsonObject result;
                result.insert(QStringLiteral("error"), QStringLiteral("E_DISABLED"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("QML property is not writable: %1")
                                  .arg(name));
                return result;
            }
            const QVariant oldValue = property.read(item);
            const QVariant newValue =
                params.value(QStringLiteral("value")).toVariant();
            if (!item->setProperty(name.toUtf8().constData(), newValue)) {
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_INVALID_ARGUMENT"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("setting QML property failed: %1")
                                  .arg(name));
                return result;
            }
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("name"), name);
            result.insert(QStringLiteral("old"), QJsonValue::fromVariant(oldValue));
            result.insert(QStringLiteral("new"),
                          QJsonValue::fromVariant(property.read(item)));
            return result;
        }
    }

    QQuickWindow* qmlWindow = nullptr;
    QQuickItem* qmlItem = nullptr;
    QString qmlError;
    QJsonArray qmlCandidateList;
    if (resolveQmlTarget(targetValue, requestNth(params),
                         &qmlWindow, &qmlItem, &qmlError,
                         &qmlCandidateList,
                         params.value(QStringLiteral("include_hidden"))
                             .toBool(false))) {
        if (!qmlItem->isVisible() &&
            !params.value(QStringLiteral("include_hidden")).toBool(false)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML target is hidden; pass "
                                         "include_hidden=true to override"));
            return result;
        }
        const QString name =
            params.value(QStringLiteral("name")).toString();
        const int index =
            qmlItem->metaObject()->indexOfProperty(name.toUtf8().constData());
        if (index < 0) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_INVALID_ARGUMENT"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("no such QML property: %1").arg(name));
            return result;
        }
        const QMetaProperty property = qmlItem->metaObject()->property(index);
        if (!property.isWritable()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_DISABLED"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML property is not writable: %1")
                              .arg(name));
            return result;
        }
        const QVariant oldValue = property.read(qmlItem);
        if (!qmlItem->setProperty(
                name.toUtf8().constData(),
                params.value(QStringLiteral("value")).toVariant())) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_INVALID_ARGUMENT"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("setting QML property failed: %1")
                              .arg(name));
            return result;
        }
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("name"), name);
        result.insert(QStringLiteral("old"), QJsonValue::fromVariant(oldValue));
        result.insert(QStringLiteral("new"),
                      QJsonValue::fromVariant(property.read(qmlItem)));
        return result;
    }
    if (targetValue.isObject() &&
        targetValue.toObject().contains(QStringLiteral("qml_id"))) {
        QJsonObject result;
        result.insert(
            QStringLiteral("error"),
            qmlError.contains(QStringLiteral("all are hidden"))
                ? QStringLiteral("E_HIDDEN")
                : (qmlError.contains(QStringLiteral("provide nth"))
                       ? QStringLiteral("E_AMBIGUOUS")
                       : QStringLiteral("E_NOT_FOUND")));
        result.insert(QStringLiteral("message"), qmlError);
        if (!qmlCandidateList.isEmpty())
            result.insert(QStringLiteral("candidates"), qmlCandidateList);
        return result;
    }

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        requestNth(params),
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
    const QString qmlMethod =
        params.value(QStringLiteral("method")).toString();
    const QString requestedSignature =
        params.value(QStringLiteral("signature")).toString();
    if (!qmlMethod.isEmpty()) {
        QQuickWindow* qmlWindow = nullptr;
        QQuickItem* qmlItem = nullptr;
        QString qmlError;
        QJsonArray candidates;
        bool isQmlTarget =
            resolveQmlTarget(params.value(QStringLiteral("target")),
                             requestNth(params), &qmlWindow, &qmlItem,
                             &qmlError, &candidates,
                             params.value(QStringLiteral("include_hidden"))
                                 .toBool(false));
        if (!isQmlTarget) {
            const QJsonValue target = params.value(QStringLiteral("target"));
            QString virtualId =
                target.isString()
                    ? target.toString()
                    : target.toObject()
                          .value(QStringLiteral("id"))
                          .toString();
            if (virtuals_.contains(virtualId)) {
                const VirtualRef ref = virtuals_.value(virtualId);
                qmlItem = qobject_cast<QQuickItem*>(ref.qmlObject.data());
                qmlWindow =
                    qobject_cast<QQuickWindow*>(ref.windowOwner.data());
                isQmlTarget = qmlItem != nullptr;
            }
        }
        if (isQmlTarget && qmlItem) {
            const QJsonArray args =
                params.value(QStringLiteral("args")).toArray();
            QMetaMethod chosen;
            for (int i = 0; i < qmlItem->metaObject()->methodCount(); ++i) {
                const QMetaMethod candidate = qmlItem->metaObject()->method(i);
                if (candidate.methodType() == QMetaMethod::Signal)
                    continue;
                if (QString::fromLatin1(candidate.name()) != qmlMethod)
                    continue;
                if (candidate.parameterCount() != args.size())
                    continue;
                if (!requestedSignature.isEmpty() &&
                    QString::fromLatin1(candidate.methodSignature()) !=
                        requestedSignature)
                    continue;
                chosen = candidate;
                break;
            }
            if (!chosen.isValid()) {
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_NOT_FOUND"));
                QString message =
                    QStringLiteral("no matching QML method: %1/%2")
                        .arg(qmlMethod)
                        .arg(args.size());
                const QStringList signatures =
                    methodSignatures(qmlItem, qmlMethod);
                if (!signatures.isEmpty())
                    message += QStringLiteral("; candidates: %1")
                                   .arg(signatures.join(QStringLiteral(", ")));
                const QString property =
                    setterPropertyName(qmlItem, qmlMethod);
                if (!property.isEmpty())
                    message += QStringLiteral(
                                   "; this looks like a property setter, "
                                   "use qt_set(name=\"%1\")")
                                   .arg(property);
                result.insert(QStringLiteral("message"), message);
                return result;
            }
            QVector<QVariant> values;
            QVector<QByteArray> typeNames;
            const QList<QByteArray> parameterTypes = chosen.parameterTypes();
            for (int i = 0; i < args.size(); ++i) {
                QVariant value = args.at(i).toVariant();
                const int typeId = chosen.parameterType(i);
                const bool enumArgument =
                    typeId != QMetaType::UnknownType &&
                    (QMetaType::typeFlags(typeId) &
                     QMetaType::IsEnumeration);
                if (typeId == QMetaType::QVariant) {
                    // QML functions expose untyped JS parameters as QVariant;
                    // QVariant::convert(QVariant) has no conversion path.
                    values.append(value);
                    typeNames.append(parameterTypes.at(i));
                    continue;
                }
                if (typeId == qMetaTypeId<QJSValue>()) {
                    QQmlEngine* engine = qmlEngine(qmlItem);
                    if (!engine) {
                        QJsonObject result;
                        result.insert(QStringLiteral("error"),
                                      QStringLiteral("E_INVALID_ARGUMENT"));
                        result.insert(QStringLiteral("message"),
                                      QStringLiteral("no QML engine for QJSValue "
                                                     "argument %1").arg(i));
                        return result;
                    }
                    value = QVariant::fromValue(
                        engine->toScriptValue(value));
                } else if ((enumArgument ||
                            typeId == QMetaType::UnknownType) &&
                           value.canConvert<int>()) {
                    value = value.toInt();
                } else if (value.userType() != typeId &&
                           !value.convert(typeId)) {
                    QJsonObject result;
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("E_INVALID_ARGUMENT"));
                    result.insert(
                        QStringLiteral("message"),
                        QStringLiteral("cannot convert QML argument %1 to %2")
                            .arg(i)
                            .arg(QString::fromLatin1(
                                chosen.parameterTypes().at(i).constData())));
                    return result;
                }
                values.append(value);
                typeNames.append(parameterTypes.at(i));
            }
            const int returnType = chosen.returnType();
            void* returnData = nullptr;
            QGenericReturnArgument returnArg;
            if (returnType != QMetaType::Void) {
                returnData = QMetaType::create(returnType);
                if (returnData)
                    returnArg = QGenericReturnArgument(chosen.typeName(),
                                                       returnData);
            }
            bool invoked = false;
            if (args.isEmpty()) {
                invoked = returnData
                              ? chosen.invoke(qmlItem, Qt::DirectConnection,
                                              returnArg)
                              : chosen.invoke(qmlItem, Qt::DirectConnection);
            } else {
                QGenericArgument generic[4];
                for (int i = 0; i < values.size(); ++i)
                    generic[i] = QGenericArgument(
                        typeNames.at(i).constData(), values.at(i).data());
                invoked = chosen.invoke(qmlItem, Qt::DirectConnection,
                                        returnArg, generic[0], generic[1],
                                        generic[2], generic[3]);
            }
            if (!invoked) {
                if (returnData)
                    QMetaType::destroy(returnType, returnData);
                QJsonObject result;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("E_INVALID_ARGUMENT"));
                result.insert(QStringLiteral("message"),
                              QStringLiteral("QML method not invokable: %1")
                                  .arg(qmlMethod));
                return result;
            }
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("method"), qmlMethod);
            result.insert(
                QStringLiteral("signature"),
                QString::fromLatin1(chosen.methodSignature()));
            result.insert(QStringLiteral("class"),
                          QString::fromLatin1(
                              qmlItem->metaObject()->className()));
            if (returnData) {
                result.insert(QStringLiteral("result"),
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                              returnValueToJson(
                                  QVariant(QMetaType(returnType), returnData)));
#else
                              returnValueToJson(
                                  QVariant(returnType, returnData)));
#endif
                QMetaType::destroy(returnType, returnData);
            }
            return result;
        }
        const QJsonValue qmlTarget =
            params.value(QStringLiteral("target"));
        if (qmlTarget.isObject() &&
            qmlTarget.toObject().contains(QStringLiteral("qml_id"))) {
            QJsonObject result;
            result.insert(
                QStringLiteral("error"),
                qmlError.contains(QStringLiteral("all are hidden"))
                    ? QStringLiteral("E_HIDDEN")
                    : QStringLiteral("E_NOT_FOUND"));
            result.insert(QStringLiteral("message"),
                          qmlError.isEmpty()
                              ? QStringLiteral("no QML item matches target")
                              : qmlError);
            if (!candidates.isEmpty())
                result.insert(QStringLiteral("candidates"), candidates);
            return result;
        }
    }

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        requestNth(params),
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
        if (!requestedSignature.isEmpty() &&
            QString::fromLatin1(candidate.methodSignature()) !=
                requestedSignature)
            continue;
        chosen = candidate;
        break;
    }
    if (!chosen.isValid()) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        QString message =
            QStringLiteral("no matching method: %1/%2")
                .arg(method)
                .arg(args.size());
        const QStringList signatures = methodSignatures(widget, method);
        if (!signatures.isEmpty())
            message += QStringLiteral("; candidates: %1")
                           .arg(signatures.join(QStringLiteral(", ")));
        const QString property = setterPropertyName(widget, method);
        if (!property.isEmpty())
            message += QStringLiteral(
                           "; this looks like a property setter, "
                           "use qt_set(name=\"%1\")")
                           .arg(property);
        result.insert(QStringLiteral("message"), message);
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
        const bool enumArgument =
            typeId != QMetaType::UnknownType &&
            (QMetaType::typeFlags(typeId) & QMetaType::IsEnumeration);
        if ((enumArgument || typeId == QMetaType::UnknownType) &&
            value.canConvert<int>()) {
            value = value.toInt();
        } else if (value.userType() != typeId && !value.convert(typeId)) {
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

    const int returnType = chosen.returnType();
    void* returnData = nullptr;
    QGenericReturnArgument returnArg;
    if (returnType != QMetaType::Void) {
        returnData = QMetaType::create(returnType);
        if (returnData)
            returnArg = QGenericReturnArgument(chosen.typeName(), returnData);
    }
    bool invoked = false;
    if (args.isEmpty()) {
        invoked = returnData
                      ? chosen.invoke(widget, Qt::DirectConnection, returnArg)
                      : chosen.invoke(widget, Qt::DirectConnection);
    } else {
        QGenericArgument arguments[4];
        for (int i = 0; i < values.size(); ++i)
            arguments[i] = QGenericArgument(typeNames.at(i).constData(),
                                            values.at(i).data());
        invoked = chosen.invoke(widget, Qt::DirectConnection, returnArg,
                                arguments[0], arguments[1], arguments[2],
                                arguments[3]);
    }
    if (!invoked) {
        if (returnData)
            QMetaType::destroy(returnType, returnData);
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_INVALID_ARGUMENT"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("method not invokable: %1").arg(method));
        return result;
    }

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("method"), method);
    result.insert(QStringLiteral("signature"),
                  QString::fromLatin1(chosen.methodSignature()));
    if (returnData) {
        result.insert(QStringLiteral("result"),
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                      returnValueToJson(
                          QVariant(QMetaType(returnType), returnData)));
#else
                      returnValueToJson(
                          QVariant(returnType, returnData)));
#endif
        QMetaType::destroy(returnType, returnData);
    }
    return result;
}

QJsonValue Server::returnValueToJson(const QVariant& value)
{
    if (!value.isValid())
        return QJsonValue(QJsonValue::Null);
    const int type = value.userType();
    if (type == qMetaTypeId<QJSValue>()) {
        const QJSValue script = value.value<QJSValue>();
        return QJsonValue::fromVariant(script.toVariant());
    }
    if (type == QMetaType::QVariant) {
        const QVariant inner = value.value<QVariant>();
        if (inner.isValid() && inner.userType() != QMetaType::QVariant)
            return returnValueToJson(inner);
    }
    const bool objectPointer =
        type == QMetaType::QObjectStar ||
        (QMetaType::typeFlags(type) & QMetaType::PointerToQObject);
    if (!objectPointer)
        return QJsonValue::fromVariant(value);

    QObject* object = value.value<QObject*>();
    if (!object)
        return QJsonValue(QJsonValue::Null);
    QJsonObject info;
    info.insert(QStringLiteral("class"),
                QString::fromLatin1(object->metaObject()->className()));
    if (auto* item = qobject_cast<QQuickItem*>(object)) {
        QQuickWindow* window = item->window();
        if (window) {
            const QRect rect =
                item->mapRectToScene(item->boundingRect()).toAlignedRect();
            info.insert(QStringLiteral("id"),
                        registerQuickQml(window, item, 0, rect,
                                         QJsonObject{}));
        }
    } else if (auto* widget = qobject_cast<QWidget*>(object)) {
        info.insert(QStringLiteral("id"), idFor(widget));
    } else if (auto* window = qobject_cast<QWindow*>(object)) {
        info.insert(QStringLiteral("id"), idForWindow(window));
    }
    return info;
}

QJsonObject Server::methods(const QJsonObject& params)
{
    QObject* object = nullptr;
    QString className;
    QString error;
    QQuickWindow* qmlWindow = nullptr;
    QQuickItem* qmlItem = nullptr;
    QJsonArray candidates;
    if (resolveQmlTarget(params.value(QStringLiteral("target")),
                         requestNth(params), &qmlWindow, &qmlItem, &error,
                         &candidates,
                         params.value(QStringLiteral("include_hidden"))
                             .toBool(false))) {
        object = qmlItem;
    } else {
        const QJsonValue target = params.value(QStringLiteral("target"));
        QString virtualId =
            target.isString()
                ? target.toString()
                : target.toObject().value(QStringLiteral("id")).toString();
        if (virtuals_.contains(virtualId)) {
            const VirtualRef ref = virtuals_.value(virtualId);
            object = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        }
        if (!object) {
            QWidget* widget = resolveTarget(target, &error, requestNth(params),
                                             params.value(QStringLiteral("include_hidden")).toBool(false));
            object = widget;
        }
    }
    if (!object) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"), error);
        if (!candidates.isEmpty())
            result.insert(QStringLiteral("candidates"), candidates);
        return result;
    }
    className = QString::fromLatin1(object->metaObject()->className());
    const bool includeInternal =
        params.value(QStringLiteral("include_internal")).toBool(false);
    QJsonArray methodList;
    const QMetaObject* meta = object->metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() == QMetaMethod::Signal)
            continue;
        const QString methodName = QString::fromLatin1(method.name());
        if (!includeInternal && methodName.startsWith(QStringLiteral("_q_")))
            continue;
        QJsonObject entry;
        entry.insert(QStringLiteral("name"),
                     QString::fromLatin1(method.name()));
        entry.insert(QStringLiteral("signature"),
                     QString::fromLatin1(method.methodSignature()));
        entry.insert(QStringLiteral("return_type"),
                     QString::fromLatin1(method.typeName()));
        QJsonArray parameterTypes;
        for (const QByteArray& parameter : method.parameterTypes())
            parameterTypes.append(QString::fromLatin1(parameter));
        entry.insert(QStringLiteral("parameter_types"), parameterTypes);
        entry.insert(QStringLiteral("slot"),
                     method.methodType() == QMetaMethod::Slot);
        methodList.append(entry);
    }
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("class"), className);
    result.insert(QStringLiteral("method_count"), methodList.size());
    result.insert(QStringLiteral("methods"), methodList);
    return result;
}

QJsonObject Server::items(const QJsonObject& params)
{
    QString error;
    const QJsonValue ownerTarget =
        params.contains(QStringLiteral("owner"))
            ? params.value(QStringLiteral("owner"))
            : params.value(QStringLiteral("target"));
    const QString ownerVirtualId =
        ownerTarget.isString()
            ? ownerTarget.toString()
            : ownerTarget.toObject().value(QStringLiteral("id")).toString();
    if (QWindow* quickWindow = windowForId(ownerVirtualId)) {
        if (auto* window = qobject_cast<QQuickWindow*>(quickWindow))
            return quickItems(window, window->contentItem(), params,
                              ownerVirtualId);
    }
    if (virtuals_.contains(ownerVirtualId)) {
        const VirtualRef ref = virtuals_.value(ownerVirtualId);
        if (ref.kind == QStringLiteral("qml_item")) {
            if (auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data())) {
                if (auto* window =
                        qobject_cast<QQuickWindow*>(ref.windowOwner.data()))
                    return quickItems(window, item, params, ownerVirtualId);
            }
        }
    }
    QWidget* owner = resolveTarget(
        ownerTarget, &error,
        requestNth(params),
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
    if (!owner && ref.windowOwner.isNull())
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

QJsonObject Server::virtualItem(const QString& id,
                                const QJsonValue& requested)
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
    if (ref.owner.isNull() && ref.windowOwner.isNull()) {
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
            // ref.props is a registration-time snapshot. Re-read QML
            // properties so get() observes type/set changes immediately.
            for (auto it = props.begin(); it != props.end(); ++it) {
                const QVariant value =
                    qml->property(it.key().toUtf8().constData());
                if (value.isValid())
                    it.value() = QJsonValue::fromVariant(value);
            }
            QStringList requestedNames;
            if (requested.isString() && requested.toString() == QStringLiteral("*")) {
                const QMetaObject* meta = qml->metaObject();
                for (int i = 0; i < meta->propertyCount(); ++i)
                    requestedNames.append(
                        QString::fromLatin1(meta->property(i).name()));
            } else if (requested.isString()) {
                requestedNames.append(requested.toString());
            } else if (requested.isArray()) {
                for (const QJsonValue& value : requested.toArray())
                    requestedNames.append(value.toString());
            }
            for (const QString& name : requestedNames) {
                const QVariant value =
                    qml->property(name.toUtf8().constData());
                if (value.isValid())
                    props.insert(name, QJsonValue::fromVariant(value));
            }
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
    if (ref.owner.isNull() && ref.windowOwner.isNull()) {
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
    const QString action = params.value(QStringLiteral("action")).toString();
    if (action != QStringLiteral("click") &&
        action != QStringLiteral("dblclick") &&
        action != QStringLiteral("key") &&
        action != QStringLiteral("type")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_IMPLEMENTED"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("only click/dblclick/key/type work on virtual items"));
        return result;
    }
    const int settleMs = params.value(QStringLiteral("settle_ms")).toInt(0);
    QRect rect;
    if (!virtualRect(ref, &rect)) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("virtual item is not visible"));
        return result;
    }

    if (action == QStringLiteral("key") &&
        !ref.windowOwner.isNull()) {
        auto* window = qobject_cast<QQuickWindow*>(ref.windowOwner.data());
        if (!window) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
            return result;
        }
        QStringList names;
        const QJsonValue keysValue = params.value(QStringLiteral("keys"));
        if (keysValue.isArray()) {
            for (const QJsonValue& value : keysValue.toArray())
                names.append(value.toString());
        } else if (!params.value(QStringLiteral("key")).toString().isEmpty()) {
            names.append(params.value(QStringLiteral("key")).toString());
        } else {
            names.append(keysValue.toString());
        }
        for (const QString& name : names) {
            if (name.isEmpty())
                continue;
            const QKeySequence sequence(name);
            if (sequence.isEmpty())
                continue;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const int key = sequence[0].key();
#else
            const int key = sequence[0];
#endif
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
            QCoreApplication::processEvents();
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
    if (action == QStringLiteral("type") &&
        ref.kind == QStringLiteral("qml_item") &&
        !ref.windowOwner.isNull()) {
        auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        if (item && !item->isVisible() &&
            !params.value(QStringLiteral("include_hidden")).toBool(false)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML target is hidden; pass "
                                         "include_hidden=true to override"));
            return result;
        }
        if (!item || item->metaObject()->indexOfProperty("text") < 0) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_NOT_EDITABLE"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML item has no text property"));
            return result;
        }
        QMetaObject::invokeMethod(item, "forceActiveFocus",
                                  Qt::DirectConnection);
        QString text = item->property("text").toString();
        if (params.value(QStringLiteral("clear")).toBool(false))
            text.clear();
        text += params.value(QStringLiteral("text")).toString();
        item->setProperty("text", text);
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("text_after"), text);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), resolved);
        return result;
    }
    if (ref.kind == QStringLiteral("qml_item") &&
        !ref.windowOwner.isNull()) {
        auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        auto* window = qobject_cast<QQuickWindow*>(ref.windowOwner.data());
        if (item && !item->isVisible() &&
            !params.value(QStringLiteral("include_hidden")).toBool(false)) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_HIDDEN"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML target is hidden; pass "
                                         "include_hidden=true to override"));
            return result;
        }
        if (!item || !window) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_STALE_ID"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("QML item is gone"));
            return result;
        }
        window->requestActivate();
        QQuickItem* clickItem = item;
        for (QQuickItem* candidate = item; candidate;
             candidate = candidate->parentItem()) {
            if ((candidate->acceptedMouseButtons() & Qt::LeftButton) ||
                candidate->inherits("QQuickAbstractButton") ||
                candidate->inherits("QQuickMouseArea")) {
                clickItem = candidate;
                break;
            }
        }
        // Keep the event at the requested item's center. The ancestor is only
        // used for focus: in Qt Quick a visible Item often has a *sibling*
        // MouseArea, so walking to an accepting ancestor would both miss that
        // sibling and move the click to an unrelated container center.
        const QRectF sourceRect = item->boundingRect();
        const QPointF point =
            (sourceRect.width() > 0 && sourceRect.height() > 0)
                ? item->mapToScene(sourceRect.center())
                : clickItem->mapToScene(clickItem->boundingRect().center());
        const int clicks = action == QStringLiteral("dblclick") ? 2 : 1;
        const QJsonObject observedBefore = propertySnapshot(item);
        clickItem->forceActiveFocus();
        for (int i = 0; i < clicks; ++i) {
            QMouseEvent move(QEvent::MouseMove, point, point,
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &move);
            QCoreApplication::processEvents();
            QMouseEvent press(QEvent::MouseButtonPress, point, point,
                              Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::processEvents();
            QMouseEvent release(QEvent::MouseButtonRelease, point, point,
                                Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
            QCoreApplication::processEvents();
        }
        const int settledMs = settlePump(settleMs);
        QJsonObject resolved;
        resolved.insert(QStringLiteral("id"), id);
        resolved.insert(QStringLiteral("class"), ref.className);
        resolved.insert(QStringLiteral("virtual"), true);
        QJsonObject observed = resolved;
        if (QObject* observedObject = ref.qmlObject.data())
            addObservedChanges(&observed, observedObject, observedBefore);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("action"), action);
        result.insert(QStringLiteral("dispatched"), true);
        result.insert(QStringLiteral("input"), QStringLiteral("mouse"));
        result.insert(QStringLiteral("settled_ms"), settledMs);
        result.insert(QStringLiteral("resolved"), resolved);
        result.insert(QStringLiteral("observed"), observed);
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
    const QJsonObject observedBefore = propertySnapshot(target);
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

    const int settledMs = settlePump(settleMs);
    QJsonObject resolved;
    resolved.insert(QStringLiteral("id"), id);
    resolved.insert(QStringLiteral("class"), ref.className);
    resolved.insert(QStringLiteral("virtual"), true);
    QJsonObject observed = resolved;
    if (!targetGuard.isNull())
        addObservedChanges(&observed, targetGuard.data(), observedBefore);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("action"), action);
    result.insert(QStringLiteral("dispatched"), true);
    result.insert(QStringLiteral("settled_ms"), settledMs);
    result.insert(QStringLiteral("resolved"), resolved);
    result.insert(QStringLiteral("observed"), observed);
    return result;
}

QJsonObject Server::quickItems(QWindow* window, QQuickItem* root,
                               const QJsonObject& params,
                               const QString& ownerId)
{
    auto* quickWindow = qobject_cast<QQuickWindow*>(window);
    if (!quickWindow || !root) {
        QJsonObject result;
        result.insert(QStringLiteral("error"), QStringLiteral("E_NOT_FOUND"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("QML owner is not available"));
        return result;
    }
    const bool includeHidden =
        params.value(QStringLiteral("include_hidden")).toBool(false);
    const bool compact = params.value(QStringLiteral("compact")).toBool(false);
    const int limit = qMax(1, params.value(QStringLiteral("limit")).toInt(50));
    const int offset = qMax(0, params.value(QStringLiteral("offset")).toInt(0));
    const QJsonObject where = params.value(QStringLiteral("where")).toObject();
    auto matches = [&](QQuickItem* item, const QString& className,
                       const QString& qmlId) {
        if (where.isEmpty())
            return true;
        if (where.contains(QStringLiteral("class")) &&
            !matchText(where.value(QStringLiteral("class")), className))
            return false;
        if (where.contains(QStringLiteral("qml_type")) &&
            !matchText(where.value(QStringLiteral("qml_type")), className))
            return false;
        if (where.contains(QStringLiteral("qml_id")) &&
            !matchText(where.value(QStringLiteral("qml_id")), qmlId))
            return false;
        if (where.contains(QStringLiteral("text")) &&
            !matchText(where.value(QStringLiteral("text")),
                       item->property("text").toString()))
            return false;
        const QJsonValue objectSpec =
            where.contains(QStringLiteral("object_name"))
                ? where.value(QStringLiteral("object_name"))
                : where.value(QStringLiteral("objectName"));
        if ((where.contains(QStringLiteral("object_name")) ||
             where.contains(QStringLiteral("objectName"))) &&
            !matchText(objectSpec, item->objectName()))
            return false;
        return true;
    };
    QJsonArray all;
    int visible = 0;
    int total = 0;
    for (QQuickItem* item : root->childItems()) {
        if (!item)
            continue;
        if (!item->isVisible() && !includeHidden)
            continue;
        ++total;
        if (!item->isVisible())
            continue;
        ++visible;
        const QString className =
            QString::fromLatin1(item->metaObject()->className());
        const QString qmlId =
            QQmlEngine::contextForObject(item)
                ? QQmlEngine::contextForObject(item)->nameForObject(item)
                : QString();
        if (!matches(item, className, qmlId))
            continue;
        const QRect rect =
            item->mapRectToScene(item->boundingRect()).toAlignedRect();
        QJsonObject props;
        props.insert(QStringLiteral("text"),
                     item->property("text").toString());
        props.insert(QStringLiteral("visible"), item->isVisible());
        props.insert(QStringLiteral("enabled"), item->isEnabled());
        const QString id =
            registerQuickQml(quickWindow, item, all.size(), rect, props);
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("kind"), QStringLiteral("qml_item"));
        entry.insert(QStringLiteral("virtual"), true);
        if (!compact) {
            entry.insert(QStringLiteral("class"), className);
            entry.insert(QStringLiteral("parent"), idForWindow(quickWindow));
            entry.insert(QStringLiteral("qml_type"), className);
            entry.insert(QStringLiteral("qml_id"), qmlId);
            entry.insert(QStringLiteral("objectName"), item->objectName());
            entry.insert(QStringLiteral("child_count"), item->childItems().size());
            entry.insert(QStringLiteral("children"), QJsonArray{});
            entry.insert(QStringLiteral("source"), QStringLiteral("adapter"));
        }
        QJsonObject r;
        r.insert(QStringLiteral("x"), rect.x());
        r.insert(QStringLiteral("y"), rect.y());
        r.insert(QStringLiteral("w"), rect.width());
        r.insert(QStringLiteral("h"), rect.height());
        entry.insert(QStringLiteral("rect"), r);
        entry.insert(QStringLiteral("props"), props);
        all.append(entry);
    }
    QJsonArray page;
    for (int i = offset; i < all.size() && i < offset + limit; ++i)
        page.append(all.at(i));
    QJsonObject result;
    result.insert(QStringLiteral("owner"), ownerId);
    result.insert(QStringLiteral("items"), page);
    result.insert(QStringLiteral("total"), total);
    result.insert(QStringLiteral("visible"), visible);
    result.insert(QStringLiteral("truncated"),
                  all.size() > offset + page.size());
    return result;
}

QJsonObject Server::source(const QJsonObject& params)
{
    const QString overrideDir =
        params.value(QStringLiteral("source_dir")).toString();
    // source_dir is exclusive: it replaces every heuristic root.
    const QStringList roots = sourceRoots(overrideDir);
    ScanBudget budget;
    const int snippetLines =
        params.value(QStringLiteral("snippet_lines")).toInt(3);
    const int maxLines =
        qMax(1, params.value(QStringLiteral("max_lines")).toInt(20));
    const QString handler =
        params.value(QStringLiteral("handler")).toString();
    UiHit hit;
    // Locate once per origin/roots/name. Misses are cached unless the scan was
    // truncated, because a retry with a wider budget may still succeed.
    auto locate = [&](const QString& origin, const QString& name,
                      const std::function<bool(UiHit*)>& scan) {
        if (name.isEmpty())
            return false;
        const QString key = sourceCacheKey(origin, roots, name);
        const auto cached = sourceMissCache.constFind(key);
        if (cached != sourceMissCache.constEnd()) {
            if (cached.value().file.isEmpty())
                return false;
            hit = cached.value();
            return true;
        }
        if (sourceMissCache.size() >= kSourceCacheLimit)
            sourceMissCache.clear();
        UiHit candidate;
        if (!scan(&candidate)) {
            if (!budget.truncated)
                sourceMissCache.insert(key, UiHit{});
            return false;
        }
        sourceMissCache.insert(key, candidate);
        hit = candidate;
        return true;
    };

    const QJsonValue sourceTarget = params.value(QStringLiteral("target"));
    QString sourceVirtualId;
    if (sourceTarget.isString()) {
        sourceVirtualId = sourceTarget.toString();
    } else if (sourceTarget.isObject()) {
        sourceVirtualId =
            sourceTarget.toObject().value(QStringLiteral("id")).toString();
    }
    QString sourceQmlId;
    if (sourceTarget.isObject())
        sourceQmlId =
            sourceTarget.toObject().value(QStringLiteral("qml_id")).toString();
    if (!sourceVirtualId.isEmpty() && !virtuals_.contains(sourceVirtualId))
        sourceQmlId = sourceVirtualId;
    if (!sourceVirtualId.isEmpty() && virtuals_.contains(sourceVirtualId))
        sourceQmlId.clear();
    if (!sourceQmlId.isEmpty()) {
        const QList<QmlItemMatch> matches = findQmlMatches(sourceQmlId);
        const int nth = requestNth(params);
        if (matches.size() > 1 && nth < 0) {
            QJsonObject result;
            result.insert(QStringLiteral("error"), QStringLiteral("E_AMBIGUOUS"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("%1 QML items have qml_id %2; "
                                         "provide nth")
                              .arg(matches.size())
                              .arg(sourceQmlId));
            return result;
        }
        if (nth >= matches.size()) {
            QJsonObject result;
            result.insert(QStringLiteral("error"),
                          QStringLiteral("E_INVALID_ARGUMENT"));
            result.insert(QStringLiteral("message"),
                          QStringLiteral("nth %1 out of range (%2)")
                              .arg(nth)
                              .arg(matches.size()));
            return result;
        }
        if (!matches.isEmpty()) {
            const QmlItemMatch match = matches.at(nth < 0 ? 0 : nth);
            const QRect rect =
                match.item->mapRectToScene(match.item->boundingRect())
                    .toAlignedRect();
            sourceVirtualId = registerQuickQml(
                match.window, match.item, 0, rect, QJsonObject{});
        }
    }
    if (!sourceVirtualId.isEmpty() && virtuals_.contains(sourceVirtualId)) {
        const VirtualRef ref = virtuals_.value(sourceVirtualId);
        auto* item = qobject_cast<QQuickItem*>(ref.qmlObject.data());
        if (ref.kind == QStringLiteral("qml_item") && item) {
            const QString queryName =
                !item->objectName().isEmpty()
                    ? item->objectName()
                    : (QQmlEngine::contextForObject(item)
                           ? QQmlEngine::contextForObject(item)
                                 ->nameForObject(item)
                           : QString());
            bool located = false;
            if (!queryName.isEmpty()) {
                QQmlContext* qmlContext =
                    QQmlEngine::contextForObject(item);
                if (qmlContext && qmlContext->baseUrl().isLocalFile()) {
                    const QString localFile =
                        qmlContext->baseUrl().toLocalFile();
                    located = locate(
                        QStringLiteral("qml-file:") + localFile, queryName,
                        [&](UiHit* out) {
                            return findQmlLocationInFile(localFile, queryName,
                                                         out);
                        });
                }
                if (!located && qmlContext) {
                    const QString fileName = qmlContext->baseUrl().fileName();
                    located = locate(
                        QStringLiteral("qml-file-name:") + fileName,
                        queryName, [&](UiHit* out) {
                            return findQmlLocationByFileName(
                                fileName, queryName, roots, out, &budget);
                        });
                }
                if (!located)
                    located = locate(
                        QStringLiteral("qml"), queryName, [&](UiHit* out) {
                            return findQmlLocation(queryName, roots, out,
                                                   &budget);
                        });
            }
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
            if (!located) {
                if (budget.truncated)
                    diagnostics.append(QStringLiteral(
                        "Source scan stopped after %1 entries (%2 ms budget) "
                        "before finding a match; pass source_dir to narrow "
                        "the search.")
                        .arg(budget.visited)
                        .arg(budget.budgetMs));
                else
                    diagnostics.append(QStringLiteral(
                        "No .qml source with this QML id/objectName was "
                        "found."));
            }
            QJsonObject result;
            result.insert(QStringLiteral("available"), located);
            if (!located)
                result.insert(QStringLiteral("code"),
                              budget.truncated
                                  ? QStringLiteral("E_SOURCE_TIMEOUT")
                                  : QStringLiteral("E_NO_DEBUGINFO"));
            result.insert(QStringLiteral("kind"), QStringLiteral("qml"));
            result.insert(QStringLiteral("target"), target);
            result.insert(QStringLiteral("location"), location);
            result.insert(QStringLiteral("qml"), qml);
            if (located) {
                const QJsonObject snippet =
                    snippetFor(hit, snippetLines, maxLines);
                if (!snippet.isEmpty())
                    result.insert(QStringLiteral("snippet"), snippet);
                if (!handler.isEmpty()) {
                    const int handlerLine = findHandlerLine(
                        hit.file, hit.line, maxLines, handler);
                    if (handlerLine > 0) {
                        result.insert(
                            QStringLiteral("handler"),
                            QJsonObject{
                                {QStringLiteral("name"), handler},
                                {QStringLiteral("line"), handlerLine}});
                    } else {
                        result.insert(QStringLiteral("code"),
                                      QStringLiteral("E_HANDLER_NOT_FOUND"));
                        diagnostics.append(QStringLiteral(
                            "E_HANDLER_NOT_FOUND: no on%1/handler: "
                            "declaration within %2 lines of the match.")
                            .arg(handler)
                            .arg(maxLines));
                    }
                }
            }
            result.insert(QStringLiteral("search"),
                          searchInfo(roots, budget));
            result.insert(QStringLiteral("diagnostics"), diagnostics);
            return result;
        }
    }

    QString error;
    QWidget* widget = resolveTarget(
        params.value(QStringLiteral("target")), &error,
        requestNth(params),
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

    QString origin = QStringLiteral("none");
    bool located =
        locate(QStringLiteral("ui"), widget->objectName(), [&](UiHit* out) {
            return findUiLocation(widget->objectName(), roots, out, &budget);
        });
    if (located) {
        origin = QStringLiteral("ui");
    } else {
        QString qmlName;
        if (auto* object = objectFromVariant(widget->property("rootObject")))
            qmlName = object->objectName();
        if (qmlName.isEmpty()) {
            if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
                if (QQuickItem* qmlRoot = quick->rootObject())
                    qmlName = qmlRoot->objectName();
            }
        }
        if (!qmlName.isEmpty() &&
            locate(QStringLiteral("qml"), qmlName, [&](UiHit* out) {
                return findQmlLocation(qmlName, roots, out, &budget);
            })) {
            located = true;
            origin = QStringLiteral("qml");
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
        if (budget.truncated) {
            diagnostics.append(QStringLiteral(
                "Source scan stopped after %1 entries (%2 ms budget) before "
                "finding a match; pass source_dir to narrow the search.")
                .arg(budget.visited)
                .arg(budget.budgetMs));
        } else {
            diagnostics.append(QStringLiteral(
                "No .ui/.qml source with this objectName was found near the "
                "executable or source root; this build exposes identity "
                "only."));
        }
    } else {
        diagnostics.append(QStringLiteral(
            "Matched a %1 source by objectName (heuristic).").arg(origin));
    }

    QJsonObject result;
    result.insert(QStringLiteral("available"), located);
    if (!located)
        result.insert(QStringLiteral("code"),
                      budget.truncated
                          ? QStringLiteral("E_SOURCE_TIMEOUT")
                          : QStringLiteral("E_NO_DEBUGINFO"));
    result.insert(QStringLiteral("kind"), QStringLiteral("widget"));
    result.insert(QStringLiteral("target"), target);
    result.insert(QStringLiteral("location"), location);
    result.insert(QStringLiteral("widget"), widgetInfo);
    if (located) {
        const QJsonObject snippet = snippetFor(hit, snippetLines, maxLines);
        if (!snippet.isEmpty())
            result.insert(QStringLiteral("snippet"), snippet);
    }
    result.insert(QStringLiteral("search"), searchInfo(roots, budget));
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
        QStringLiteral("props"), QStringLiteral("inherits")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key())) {
            lastResolveCode_ = QStringLiteral("E_INVALID_FILTER");
            if (error)
                *error = QStringLiteral(
                             "unsupported locator key: %1 (supported: id, "
                             "class, object_name/objectName, text, props, "
                             "inherits)")
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

QString stripQmlTypeSuffix(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("_QMLTYPE_\\d+")),
                 QString());
    name.replace(QRegularExpression(QStringLiteral("_QML_\\d+")), QString());
    return name;
}

QString qmlTypeName(const QQuickItem* item)
{
    return stripQmlTypeSuffix(
        QString::fromLatin1(item->metaObject()->className()));
}

bool qmlMatchInherits(const QJsonValue& spec, const QQuickItem* item)
{
    if (!item || !validMatcher(spec))
        return false;
    for (const QMetaObject* meta = item->metaObject(); meta;
         meta = meta->superClass()) {
        const QString name = stripQmlTypeSuffix(
            QString::fromLatin1(meta->className()));
        if (!name.isEmpty() && matchText(spec, name))
            return true;
    }
    return false;
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
        QStringLiteral("props"), QStringLiteral("inherits")};
    for (auto it = where.begin(); it != where.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    const QString className =
        QString::fromLatin1(item->metaObject()->className());
    if (where.contains(QStringLiteral("class")) &&
        !matchText(where.value(QStringLiteral("class")), className))
        return false;
    if (where.contains(QStringLiteral("inherits")) &&
        !qmlMatchInherits(where.value(QStringLiteral("inherits")), item))
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
    if (context.maxDepth >= 0 && depth > context.maxDepth)
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
        if (!child)
            continue;
        if (!child->isVisible()) {
            if (!context.includeHidden)
                ++context.hiddenPruned;
            continue;
        }
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
    const QString qmlType = qmlTypeName(item);
    const QString qmlId = qmlIdName(item);
    node.insert(QStringLiteral("id"),
                registerQml(owner, item, (*serial)++, rect, props));
    node.insert(QStringLiteral("class"), qmlType);
    if (!context.compact || !qmlType.isEmpty())
        node.insert(QStringLiteral("qml_type"), qmlType);
    if (!context.compact || !qmlId.isEmpty())
        node.insert(QStringLiteral("qml_id"), qmlId);
    node.insert(QStringLiteral("objectName"), item->objectName());
    node.insert(QStringLiteral("qml"), true);
    if (!context.compact)
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
    if (!context.compact || childCount > 0)
        node.insert(QStringLiteral("child_count"), childCount);
    if (!context.compact || !children.isEmpty())
        node.insert(QStringLiteral("children"), children);
    node.insert(QStringLiteral("props"), props);
    if (context.hasWhere)
        node.insert(QStringLiteral("matched"), selfMatch);
    return node;
}

QString Server::registerQuickQml(QQuickWindow* window, QQuickItem* item,
                                 int index, const QRect& rect,
                                 const QJsonObject& props)
{
    const QString id = tag_ + QStringLiteral(".vqml.") +
                       idForWindow(window) + QStringLiteral(".") +
                       QString::number(reinterpret_cast<quintptr>(item), 16);
    VirtualRef ref;
    ref.windowOwner = window;
    ref.kind = QStringLiteral("qml_item");
    ref.className = qmlTypeName(item);
    ref.index = index;
    ref.rect = rect;
    ref.props = props;
    ref.qmlObject = item;
    virtuals_.insert(id, ref);
    return id;
}

QJsonObject Server::buildQuickItemNode(QQuickWindow* window, QQuickItem* item,
                                       QueryContext& context, int depth,
                                       int* serial, bool* anyMatch)
{
    *anyMatch = false;
    if (!item)
        return {};
    if (context.maxDepth >= 0 && depth > context.maxDepth)
        return {};
    const bool selfMatch =
        context.hasWhere && qmlMatches(item, context.where);
    ++context.total;
    if (selfMatch)
        ++context.matched;
    const QRect rect =
        item->mapRectToScene(item->boundingRect()).toAlignedRect();
    const QJsonObject props = qmlProps(item, context.props);
    int childCount = 0;
    QJsonArray children;
    bool childAny = false;
    if ((context.includeHidden || item->isVisible()) &&
        context.emitted < QueryContext::kMaxEmitted) {
        for (QQuickItem* child : item->childItems()) {
            if (!child)
                continue;
            if (!context.includeHidden && !child->isVisible()) {
                ++context.hiddenPruned;
                continue;
            }
            bool childMatch = false;
            const QJsonObject node =
                buildQuickItemNode(window, child, context, depth + 1, serial,
                                   &childMatch);
            if (childMatch) {
                childAny = true;
                children.append(node);
                ++childCount;
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

    const QString id = registerQuickQml(window, item, (*serial)++, rect, props);
    const QString qmlType = qmlTypeName(item);
    const QString qmlId = qmlIdName(item);
    QJsonObject node;
    node.insert(QStringLiteral("id"), id);
    node.insert(QStringLiteral("class"),
                QString::fromLatin1(item->metaObject()->className()));
    if (!context.compact || !qmlType.isEmpty())
        node.insert(QStringLiteral("qml_type"), qmlType);
    if (!context.compact || !qmlId.isEmpty())
        node.insert(QStringLiteral("qml_id"), qmlId);
    node.insert(QStringLiteral("objectName"), item->objectName());
    node.insert(QStringLiteral("qml"), true);
    if (!context.compact)
        node.insert(QStringLiteral("window"), false);
    if (!context.compact || childCount > 0)
        node.insert(QStringLiteral("child_count"), childCount);
    if (!context.compact || !children.isEmpty())
        node.insert(QStringLiteral("children"), children);
    node.insert(QStringLiteral("props"), props);
    node.insert(QStringLiteral("rect"), QJsonObject{
        {QStringLiteral("x"), rect.x()}, {QStringLiteral("y"), rect.y()},
        {QStringLiteral("w"), rect.width()}, {QStringLiteral("h"), rect.height()}});
    if (context.screenCoords) {
        const QPoint screen = window->mapToGlobal(rect.topLeft());
        node.insert(QStringLiteral("screen_rect"), QJsonObject{
            {QStringLiteral("x"), screen.x()}, {QStringLiteral("y"), screen.y()},
            {QStringLiteral("w"), rect.width()}, {QStringLiteral("h"), rect.height()}});
    }
    if (context.hasWhere)
        node.insert(QStringLiteral("matched"), selfMatch);
    return node;
}

QJsonObject Server::buildQuickWindowNode(QQuickWindow* window,
                                         QueryContext& context, bool isRoot,
                                         bool* anyMatch)
{
    *anyMatch = false;
    if (!window)
        return {};
    const QString windowClass = QStringLiteral("QQuickWindow");
    bool selfMatch = false;
    if (context.hasWhere) {
        if (context.where.contains(QStringLiteral("class")))
            selfMatch = matchText(context.where.value(QStringLiteral("class")),
                                  windowClass);
        else if (context.where.contains(QStringLiteral("object_name")) ||
                 context.where.contains(QStringLiteral("objectName")))
            selfMatch = matchText(
                context.where.contains(QStringLiteral("object_name"))
                    ? context.where.value(QStringLiteral("object_name"))
                    : context.where.value(QStringLiteral("objectName")),
                window->objectName());
        else if (context.where.contains(QStringLiteral("text")))
            selfMatch = matchText(context.where.value(QStringLiteral("text")),
                                  window->title());
        else if (context.where.contains(QStringLiteral("inherits")))
            selfMatch = matchInherits(
                context.where.value(QStringLiteral("inherits")), window);
        else
            selfMatch = false;
    }
    ++context.total;
    if (selfMatch)
        ++context.matched;

    QJsonArray children;
    bool childAny = false;
    int childCount = 0;
    if (window->contentItem() &&
        context.emitted < QueryContext::kMaxEmitted) {
        for (QQuickItem* child : window->contentItem()->childItems()) {
            if (!child)
                continue;
            if (!context.includeHidden && !child->isVisible()) {
                ++context.hiddenPruned;
                continue;
            }
            bool childMatch = false;
            int serial = 0;
            const QJsonObject node =
                buildQuickItemNode(window, child, context, 1, &serial,
                                   &childMatch);
            if (childMatch) {
                childAny = true;
                children.append(node);
                ++childCount;
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
    node.insert(QStringLiteral("id"), idForWindow(window));
    node.insert(QStringLiteral("class"), windowClass);
    if (!context.compact) {
        node.insert(QStringLiteral("qml_type"), QJsonValue::Null);
        node.insert(QStringLiteral("qml_id"), QString());
    }
    node.insert(QStringLiteral("objectName"), window->objectName());
    node.insert(QStringLiteral("qml"), true);
    node.insert(QStringLiteral("window"), true);
    node.insert(QStringLiteral("minimized"),
                window->windowStates().testFlag(Qt::WindowMinimized));
    if (!context.compact || childCount > 0)
        node.insert(QStringLiteral("child_count"), childCount);
    if (!context.compact || !children.isEmpty())
        node.insert(QStringLiteral("children"), children);
    node.insert(QStringLiteral("rect"), QJsonObject{
        {QStringLiteral("x"), 0}, {QStringLiteral("y"), 0},
        {QStringLiteral("w"), window->width()}, {QStringLiteral("h"), window->height()}});
    const QPoint screen = window->position();
    node.insert(QStringLiteral("screen_pos"), QJsonObject{
        {QStringLiteral("x"), screen.x()}, {QStringLiteral("y"), screen.y()}});
    if (context.screenCoords)
        node.insert(QStringLiteral("screen_rect"), QJsonObject{
            {QStringLiteral("x"), screen.x()}, {QStringLiteral("y"), screen.y()},
            {QStringLiteral("w"), window->width()}, {QStringLiteral("h"), window->height()}});
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
        if (!child)
            continue;
        if (!isCandidate(child, context.includeHidden)) {
            ++context.hiddenPruned;
            continue;
        }
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
    if (!context.compact) {
        node.insert(QStringLiteral("qml_type"), QJsonValue::Null);
        node.insert(QStringLiteral("qml_id"), QStringLiteral(""));
    }
    node.insert(QStringLiteral("objectName"), widget->objectName());

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
    if (!context.compact || window)
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
    if (!context.compact || childCount > 0)
        node.insert(QStringLiteral("child_count"), childCount);
    if (!context.compact || !childNodes.isEmpty())
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
        if (!child)
            continue;
        if (!isCandidate(child, context.includeHidden)) {
            ++context.hiddenPruned;
            continue;
        }
        countNode(child, context, depth + 1);
    }
    if (auto* quick = qobject_cast<QQuickWidget*>(widget)) {
        std::function<void(QQuickItem*, int)> countQml =
            [&](QQuickItem* item, int qmlDepth) {
                if (!item)
                    return;
                if (!context.includeHidden && !item->isVisible()) {
                    ++context.hiddenPruned;
                    return;
                }
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
        QStringLiteral("text"), QStringLiteral("props"),
        QStringLiteral("inherits")};
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
    if (where.contains(QStringLiteral("inherits")) &&
        !matchInherits(where.value(QStringLiteral("inherits")), widget))
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

QString Server::idForWindow(QWindow* window)
{
    if (!window)
        return {};
    auto it = windowIds_.find(window);
    if (it != windowIds_.end()) {
        if (!idToWindow_.value(it.value()).isNull())
            return it.value();
        staleIds_.insert(it.value());
        idToWindow_.remove(it.value());
        windowIds_.erase(it);
    }
    const QString id = tag_ + QStringLiteral(".qw") +
                       QString::number(nextId_++);
    windowIds_.insert(window, id);
    idToWindow_.insert(id, QPointer<QWindow>(window));
    connect(window, &QObject::destroyed, this, [this, id, window]() {
        windowIds_.remove(window);
        idToWindow_.remove(id);
        staleIds_.insert(id);
    });
    return id;
}

QWindow* Server::windowForId(const QString& id) const
{
    return idToWindow_.value(id).data();
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


