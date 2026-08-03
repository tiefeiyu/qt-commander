#include "screenshot.h"
#include "../compat_qt.h"

#include <QWidget>
#include <QGuiApplication>
#include <QWindow>
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickItem>
#include <QQuickWindow>
// QQuickItemGrabResult header differs between Qt5 and Qt6.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  include <QQuickItemGrabResult>
#else
#  include <QtQuick/QQuickItemGrabResult>
#endif
#endif
#include <QScreen>
#include <QPixmap>
#include <QImage>
#include <QDir>
#include <QUuid>
#include <QEventLoop>
#include <QTimer>

// ============================================================================
// capture  --  takes a PNG screenshot of the given element
// ============================================================================
QString Screenshot::capture(QObject* target,
                             const QString& screenshot_dir,
                             int sequence)
{
    if (!target)
        return {};

    // Ensure the output directory exists.
    if (!screenshot_dir.isEmpty()) {
        QDir().mkpath(screenshot_dir);
    }

    // Build a unique filename: {sequence}_{uuid}.png
    const QString seqStr = QStringLiteral("%1").arg(sequence, 6, 10, QLatin1Char('0'));
    // QUuid::toString(Format) was added in Qt 5.11; use string manipulation for
    // compatibility with all Qt 5.x versions.
    const QString uuidStr = QUuid::createUuid().toString(); // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
    const QString uuid = uuidStr.mid(1, 8);                // extract first 8 hex chars
    const QString fileName = seqStr + QStringLiteral("_") + uuid + QStringLiteral(".png");
    const QString filePath = screenshot_dir + QStringLiteral("/") + fileName;

    // ----------------------------------------------------------------
    // 1) QWidget
    // ----------------------------------------------------------------
    if (auto* w = qobject_cast<QWidget*>(target)) {
        // QWidget::grab() renders offscreen and works even when the widget
        // has not been painted yet -- paintEngine() can legitimately be null
        // for a visible top-level window that has not drawn its first frame.
        // Only bail when the widget has no window at all: there is nothing
        // to render then.
        QWidget* grabTarget = w;
        if (!w->paintEngine()) {
            QWidget* win = w->window();
            if (!win)
                return {};
            grabTarget = win;
        }

        // Zero-size check.
        if (grabTarget->geometry().width() <= 0 ||
            grabTarget->geometry().height() <= 0)
            return {};

        QPixmap pm = grabTarget->grab();
        if (pm.isNull())
            return {};

        return pm.save(filePath, "PNG") ? filePath : QString();
    }

    // ----------------------------------------------------------------
    // 2) QQuickItem (needs async grabToImage with event loop wait)
    // ----------------------------------------------------------------
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        // Zero-size check.
        if (item->width() <= 0.0 || item->height() <= 0.0)
            return {};

        QQuickWindow* win = item->window();
        if (!win)
            return {};

        // grabToImage returns a QQuickItemGrabResult* with a ready signal.
        QSharedPointer<QQuickItemGrabResult> grabResult = item->grabToImage();

        if (!grabResult)
            return {};

        // Wait synchronously for the grab to finish.
        QEventLoop loop;
        QObject::connect(grabResult.data(), &QQuickItemGrabResult::ready,
                         &loop, &QEventLoop::quit);

        // Safety timeout (5 seconds).
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout,
                         &loop, &QEventLoop::quit);

        timer.start(5000);
        loop.exec();

        QImage img = grabResult->image();
        if (img.isNull())
            return {};

        return img.save(filePath, "PNG") ? filePath : QString();
    }
#endif

    // ----------------------------------------------------------------
    // 3) QWindow
    // ----------------------------------------------------------------
    if (auto* win = qobject_cast<QWindow*>(target)) {
        // Zero-size check.
        if (win->width() <= 0 || win->height() <= 0)
            return {};

        QScreen* screen = win->screen();
        if (!screen) {
            // Fallback to primary screen.
            screen = QGuiApplication::primaryScreen();
        }
        if (!screen)
            return {};

        QPixmap pm = screen->grabWindow(win->winId());
        if (pm.isNull())
            return {};

        return pm.save(filePath, "PNG") ? filePath : QString();
    }

    // ----------------------------------------------------------------
    // 4) Unknown type -- try generic property-based approach
    // ----------------------------------------------------------------
    // Last resort: check if the object has a winId() method (QObject not
    // directly QWindow, but may wrap one).
    if (auto* genericWin = qobject_cast<QObject*>(target)) {
        // Check if we can get a window handle through surface.
        auto* surface = dynamic_cast<QSurface*>(target);
        if (surface) {
            QScreen* screen = QGuiApplication::primaryScreen();
            if (screen) {
                QPixmap pm = screen->grabWindow(0);
                if (!pm.isNull())
                    return pm.save(filePath, "PNG") ? filePath : QString();
            }
        }
    }

    return {};
}
