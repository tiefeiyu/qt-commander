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
        // QML's z property is the real stacking level *between siblings*
        // (higher z paints above; negative z paints under the parent's
        // content).  The previous code returned the sibling index, which
        // silently dropped every explicit z value (e.g. z: -1 came back
        // as a positive index) and broke occlusion solving.  The solver
        // orders siblings by (z, declaration order) itself; here we must
        // report the real z.
        return static_cast<int>(item->z());
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
