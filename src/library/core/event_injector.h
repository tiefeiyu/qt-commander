#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QHash>

class QWidget;
class QQuickItem;
class QTouchDevice;

// Injects Qt events into the target application's event loop.
class EventInjector {
public:
    // Mouse operations. x,y are relative to element center if hasCoords=false.
    static bool mouseClick(QObject* target, const QString& button,
                           double x, double y,
                           const QStringList& modifiers, bool hasCoords);
    static bool mousePress(QObject* target, const QString& button,
                           double x, double y,
                           const QStringList& modifiers, bool hasCoords);
    static bool mouseRelease(QObject* target, const QString& button,
                             double x, double y,
                             const QStringList& modifiers, bool hasCoords);
    static bool mouseDblClick(QObject* target, const QString& button,
                              double x, double y,
                              const QStringList& modifiers, bool hasCoords);
    static bool mouseMove(QObject* target, double x, double y);
    static bool mouseWheel(QObject* target, double deltaX, double deltaY,
                           double x, double y, bool pixelDelta, bool hasCoords);

    // Keyboard
    static bool keyPress(QObject* target, const QString& key,
                         const QStringList& modifiers, const QString& text);
    static bool keyRelease(QObject* target, const QString& key,
                           const QStringList& modifiers, const QString& text);
    static bool typeText(QObject* target, const QString& text, int intervalMs);
    static bool keyCombo(QObject* target, const QString& keys);

    // Touch
    static bool touchPress(QObject* target, double x, double y,
                           int touchId, double pressure);
    static bool touchMove(QObject* target, double x, double y,
                          int touchId, double pressure);
    static bool touchRelease(int touchId);

    // Focus
    static bool focus(QObject* target);
    static bool clearFocus(QObject* target);

    // Context menu
    static bool contextMenu(QObject* target, double x, double y, bool hasCoords);

private:
    static Qt::MouseButton parseButton(const QString& button);
    static Qt::KeyboardModifiers parseModifiers(const QStringList& modifiers);
    static int parseKey(const QString& key);
    static void getEventPos(QObject* target, double& x, double& y, bool hasCoords);
    static QPointF elementCenter(QObject* target);
    static void sendToWidget(QWidget* widget, QEvent* event);
    static void sendToQmlItem(QQuickItem* item, QEvent* event);
    static QTouchDevice* getTouchDevice();

    // Dispatch helper: route to QWidget or QQuickItem.
    static bool dispatchEvent(QObject* target, QEvent* event);

    // Track touch point -> target mapping for touchRelease.
    struct TouchTarget {
        QObject* obj;
    };
    static QHash<int, TouchTarget> s_touchTargets;
};
