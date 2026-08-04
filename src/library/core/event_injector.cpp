#include "event_injector.h"
#include "../compat_qt.h"

#include <QCoreApplication>
#include <QApplication>
#include <QWidget>
#include <QAbstractItemView>
#include <QWindow>
#ifdef QT_COMMANDER_WITH_QML
#include <QQuickWindow>
#include <QQuickItem>
#endif
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTouchEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#include <QInputDevice>
#else
#include <QTouchDevice>
#endif
#include <QContextMenuEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
// Qt 6.4 removed QEventPoint's public setters (setPos/setState/...);
// QMutableEventPoint (private header) is the supported mutation path.
#include <QtGui/private/qeventpoint_p.h>
#endif
#include <QThread>
#include <QPoint>
#include <QHash>

// The QPA input interface: the same entry point the platform plugins use to
// deliver OS mouse events into Qt.  Events queued here are delivered on the
// GUI thread with the real hit testing (scene graph for QML windows, widget
// tree for widget windows), so an injected click behaves exactly like a
// real mouse click at the given coordinate.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// Qt6: the QPA interface lives in the private qpa header.
#include <QtGui/qpa/qwindowsysteminterface_p.h>
#else
#include <QtGui/qpa/qwindowsysteminterface.h>
#endif

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ============================================================================
// Module-level state
// ============================================================================
// Touch point tracking for touchRelease.
QHash<int, EventInjector::TouchTarget> EventInjector::s_touchTargets;

namespace {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPointingDevice* s_touchDevice = nullptr;
#else
    QTouchDevice* s_touchDevice = nullptr;
#endif

// Mouse button state across press/move/release sequences.  Real mouse move
// events carry the pressed-button state, and QApplication::notify drops
// button-less MouseMove events for widgets without mouse tracking -- so
// drags (press -> move -> release) must mark the move events with the
// currently held button.  All EventInjector calls run on the GUI thread
// (the protocol layer dispatches there), so no locking is needed.
Qt::MouseButtons s_pressedMouseButtons = Qt::NoButton;
} // anonymous namespace

// ============================================================================
// Mouse helpers
// ============================================================================

Qt::MouseButton EventInjector::parseButton(const QString& button)
{
    const QString b = button.toLower();
    if (b == QStringLiteral("left"))
        return Qt::LeftButton;
    if (b == QStringLiteral("right"))
        return Qt::RightButton;
    if (b == QStringLiteral("middle"))
        return Qt::MiddleButton;
    if (b == QStringLiteral("back"))
        return Qt::BackButton;
    if (b == QStringLiteral("forward"))
        return Qt::ForwardButton;
    if (b == QStringLiteral("xbutton1"))
        return Qt::XButton1;
    if (b == QStringLiteral("xbutton2"))
        return Qt::XButton2;
    if (b == QStringLiteral("taskbutton"))
        return Qt::TaskButton;
    if (b == QStringLiteral("extrabutton4"))
        return Qt::ExtraButton4;
    if (b == QStringLiteral("extrabutton5"))
        return Qt::ExtraButton5;
    if (b == QStringLiteral("extrabutton6"))
        return Qt::ExtraButton6;
    if (b == QStringLiteral("extrabutton7"))
        return Qt::ExtraButton7;
    if (b == QStringLiteral("extrabutton8"))
        return Qt::ExtraButton8;
    if (b == QStringLiteral("extrabutton9"))
        return Qt::ExtraButton9;
    if (b == QStringLiteral("extrabutton10"))
        return Qt::ExtraButton10;
    if (b == QStringLiteral("extrabutton11"))
        return Qt::ExtraButton11;
    if (b == QStringLiteral("extrabutton12"))
        return Qt::ExtraButton12;
    if (b == QStringLiteral("extrabutton13"))
        return Qt::ExtraButton13;
    if (b == QStringLiteral("extrabutton14"))
        return Qt::ExtraButton14;
    if (b == QStringLiteral("extrabutton15"))
        return Qt::ExtraButton15;
    if (b == QStringLiteral("extrabutton16"))
        return Qt::ExtraButton16;
    if (b == QStringLiteral("extrabutton17"))
        return Qt::ExtraButton17;
    if (b == QStringLiteral("extrabutton18"))
        return Qt::ExtraButton18;
    if (b == QStringLiteral("extrabutton19"))
        return Qt::ExtraButton19;
    if (b == QStringLiteral("extrabutton20"))
        return Qt::ExtraButton20;
    if (b == QStringLiteral("extrabutton21"))
        return Qt::ExtraButton21;
    if (b == QStringLiteral("extrabutton22"))
        return Qt::ExtraButton22;
    if (b == QStringLiteral("extrabutton23"))
        return Qt::ExtraButton23;
    if (b == QStringLiteral("extrabutton24"))
        return Qt::ExtraButton24;

    // Default to left button.
    return Qt::LeftButton;
}

Qt::KeyboardModifiers EventInjector::parseModifiers(const QStringList& modifiers)
{
    Qt::KeyboardModifiers mods;
    for (const QString& m : modifiers) {
        const QString mm = m.toLower();
        if (mm == QStringLiteral("shift"))
            mods |= Qt::ShiftModifier;
        else if (mm == QStringLiteral("ctrl") || mm == QStringLiteral("control"))
            mods |= Qt::ControlModifier;
        else if (mm == QStringLiteral("alt"))
            mods |= Qt::AltModifier;
        else if (mm == QStringLiteral("meta"))
            mods |= Qt::MetaModifier;
        else if (mm == QStringLiteral("keypad"))
            mods |= Qt::KeypadModifier;
        else if (mm == QStringLiteral("group_switch"))
            mods |= Qt::GroupSwitchModifier;
    }
    return mods;
}

int EventInjector::parseKey(const QString& key)
{
    if (key.isEmpty())
        return Qt::Key_unknown;

    // Single printable character.
    if (key.length() == 1) {
        const QChar c = key.at(0);
        if (c.isLetter())
            return static_cast<int>(Qt::Key_A + (c.toUpper().toLatin1() - 'A'));
        if (c.isDigit())
            return static_cast<int>(Qt::Key_0 + (c.toLatin1() - '0'));
        switch (c.unicode()) {
        case ' ':   return Qt::Key_Space;
        case '.':   return Qt::Key_Period;
        case ',':   return Qt::Key_Comma;
        case '-':   return Qt::Key_Minus;
        case '+':   return Qt::Key_Plus;
        case '=':   return Qt::Key_Equal;
        case '/':   return Qt::Key_Slash;
        case '\\':  return Qt::Key_Backslash;
        case ';':   return Qt::Key_Semicolon;
        case '\'':  return Qt::Key_Apostrophe;
        case '[':   return Qt::Key_BracketLeft;
        case ']':   return Qt::Key_BracketRight;
        case '`':   return Qt::Key_QuoteLeft;
        case '\n':  return Qt::Key_Return;
        case '\t':  return Qt::Key_Tab;
        case '\b':  return Qt::Key_Backspace;
        case ':':   return Qt::Key_Colon;
        case '_':   return Qt::Key_Underscore;
        case '"':   return Qt::Key_QuoteDbl;
        case '<':   return Qt::Key_Less;
        case '>':   return Qt::Key_Greater;
        case '?':   return Qt::Key_Question;
        case '!':   return Qt::Key_Exclam;
        case '@':   return Qt::Key_At;
        case '#':   return Qt::Key_NumberSign;
        case '$':   return Qt::Key_Dollar;
        case '%':   return Qt::Key_Percent;
        case '^':   return Qt::Key_AsciiCircum;
        case '&':   return Qt::Key_Ampersand;
        case '*':   return Qt::Key_Asterisk;
        case '(':   return Qt::Key_ParenLeft;
        case ')':   return Qt::Key_ParenRight;
        case '{':   return Qt::Key_BraceLeft;
        case '}':   return Qt::Key_BraceRight;
        case '|':   return Qt::Key_Bar;
        case '~':   return Qt::Key_AsciiTilde;
        default:    return c.unicode();
        }
    }

    // Named keys via manual lookup table (no QMetaEnum dependency).
    struct KeyEntry { const char* name; int key; };
    static const KeyEntry keyTable[] = {
        {"Return",        Qt::Key_Return},
        {"Enter",         Qt::Key_Enter},
        {"Tab",           Qt::Key_Tab},
        {"Space",         Qt::Key_Space},
        {"Escape",        Qt::Key_Escape},
        {"Backspace",     Qt::Key_Backspace},
        {"Delete",        Qt::Key_Delete},
        {"Home",          Qt::Key_Home},
        {"End",           Qt::Key_End},
        {"PageUp",        Qt::Key_PageUp},
        {"PageDown",      Qt::Key_PageDown},
        {"Left",          Qt::Key_Left},
        {"Right",         Qt::Key_Right},
        {"Up",            Qt::Key_Up},
        {"Down",          Qt::Key_Down},
        {"Insert",        Qt::Key_Insert},
        {"Pause",         Qt::Key_Pause},
        {"Print",         Qt::Key_Print},
        {"SysReq",        Qt::Key_SysReq},
        {"Menu",          Qt::Key_Menu},
        {"Help",          Qt::Key_Help},
        {"Shift",         Qt::Key_Shift},
        {"Control",       Qt::Key_Control},
        {"Alt",           Qt::Key_Alt},
        {"Meta",          Qt::Key_Meta},
        {"CapsLock",      Qt::Key_CapsLock},
        {"NumLock",       Qt::Key_NumLock},
        {"ScrollLock",    Qt::Key_ScrollLock},
        {"Clear",         Qt::Key_Clear},
        {"Cancel",        Qt::Key_Cancel},
        {"Select",        Qt::Key_Select},
        {"Execute",       Qt::Key_Execute},
        {"Undo",          Qt::Key_Undo},
        {"Redo",          Qt::Key_Redo},
        {"Find",          Qt::Key_Find},
        // Key_Replace was removed in Qt 5.15 (gone from both 5.15 and 6.x).
        {"Cut",           Qt::Key_Cut},
        {"Copy",          Qt::Key_Copy},
        {"Paste",         Qt::Key_Paste},
        {"VolumeUp",      Qt::Key_VolumeUp},
        {"VolumeDown",    Qt::Key_VolumeDown},
        {"VolumeMute",    Qt::Key_VolumeMute},
        {"MediaPlay",     Qt::Key_MediaPlay},
        {"MediaStop",     Qt::Key_MediaStop},
        {"MediaPause",    Qt::Key_MediaPause},
        {"MediaTogglePlayPause", Qt::Key_MediaTogglePlayPause},
        {"MediaRecord",   Qt::Key_MediaRecord},
        {"MediaPrevious", Qt::Key_MediaPrevious},
        {"MediaNext",     Qt::Key_MediaNext},
        // Key_MediaRewind / Key_MediaFastForward were removed in Qt 5.15.
        {"Back",          Qt::Key_Back},
        {"Forward",       Qt::Key_Forward},
        {"Refresh",       Qt::Key_Refresh},
        {"Stop",          Qt::Key_Stop},
        {"Search",        Qt::Key_Search},
        {"Favorites",     Qt::Key_Favorites},
        {"HomePage",      Qt::Key_HomePage},
        {"LaunchMail",    Qt::Key_LaunchMail},
        {"LaunchMedia",   Qt::Key_LaunchMedia},
        {"Launch0",       Qt::Key_Launch0},
        {"Launch1",       Qt::Key_Launch1},
        {"Launch2",       Qt::Key_Launch2},
        {"Launch3",       Qt::Key_Launch3},
        {"Launch4",       Qt::Key_Launch4},
        {"Launch5",       Qt::Key_Launch5},
        {"Launch6",       Qt::Key_Launch6},
        {"Launch7",       Qt::Key_Launch7},
        {"Launch8",       Qt::Key_Launch8},
        {"Launch9",       Qt::Key_Launch9},
        {"LaunchA",       Qt::Key_LaunchA},
        {"LaunchB",       Qt::Key_LaunchB},
        {"LaunchC",       Qt::Key_LaunchC},
        {"LaunchD",       Qt::Key_LaunchD},
        {"LaunchE",       Qt::Key_LaunchE},
        {"LaunchF",       Qt::Key_LaunchF},
        {"MonBrightnessUp",   Qt::Key_MonBrightnessUp},
        {"MonBrightnessDown", Qt::Key_MonBrightnessDown},
        {"KeyboardLightOnOff",  Qt::Key_KeyboardLightOnOff},
        {"KeyboardBrightnessUp", Qt::Key_KeyboardBrightnessUp},
        {"KeyboardBrightnessDown", Qt::Key_KeyboardBrightnessDown},
        {"PowerOff",      Qt::Key_PowerOff},
        {"WakeUp",        Qt::Key_WakeUp},
        {"Eject",         Qt::Key_Eject},
        {"Sleep",         Qt::Key_Sleep},
        {nullptr, 0}
    };

    // F1..F24
    static const int fKeys[] = {
        Qt::Key_F1,  Qt::Key_F2,  Qt::Key_F3,  Qt::Key_F4,
        Qt::Key_F5,  Qt::Key_F6,  Qt::Key_F7,  Qt::Key_F8,
        Qt::Key_F9,  Qt::Key_F10, Qt::Key_F11, Qt::Key_F12,
        Qt::Key_F13, Qt::Key_F14, Qt::Key_F15, Qt::Key_F16,
        Qt::Key_F17, Qt::Key_F18, Qt::Key_F19, Qt::Key_F20,
        Qt::Key_F21, Qt::Key_F22, Qt::Key_F23, Qt::Key_F24
    };

    // Check F-keys first (F1..F99).
    if (key.length() >= 2 && key.length() <= 3 && (key.at(0) == QLatin1Char('F') || key.at(0) == QLatin1Char('f'))) {
        bool ok = false;
        int num = key.mid(1).toInt(&ok);
        if (ok && num >= 1 && num <= 24)
            return fKeys[num - 1];
    }

    // Lookup named key.
    for (int i = 0; keyTable[i].name; ++i) {
        if (key.compare(QLatin1String(keyTable[i].name), Qt::CaseInsensitive) == 0)
            return keyTable[i].key;
    }

    return Qt::Key_unknown;
}

// ============================================================================
// Coordinate helpers
// ============================================================================

QPointF EventInjector::elementCenter(QObject* target)
{
    if (auto* w = qobject_cast<QWidget*>(target)) {
        QRect geo = w->geometry();
        return QPointF(geo.width() / 2.0, geo.height() / 2.0);
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        return QPointF(item->width() / 2.0, item->height() / 2.0);
    }
#endif
    return QPointF(0, 0);
}

void EventInjector::getEventPos(QObject* target,
                                 double& x, double& y,
                                 bool hasCoords)
{
    if (!hasCoords) {
        QPointF center = elementCenter(target);
        x = center.x();
        y = center.y();
    }
}

// ============================================================================
// Dispatch helpers
// ============================================================================

void EventInjector::sendToWidget(QWidget* widget, QEvent* event)
{
    QCoreApplication::postEvent(widget, event);
}

#ifdef QT_COMMANDER_WITH_QML
void EventInjector::sendToQmlItem(QQuickItem* item, QEvent* event)
{
    QQuickWindow* win = item->window();
    if (win) {
        // Qt5: QQuickWindow::sendEvent delivers straight to the item and
        // does NOT take ownership — we must delete the event afterwards.
        // Qt6: QQuickWindow::sendEvent was removed; QCoreApplication::
        // sendEvent delivers synchronously to the item's event handler
        // (QQuickItem::event routes mouse/touch/key events to the
        // corresponding QQuickItem handler).  Same ownership rule.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QCoreApplication::sendEvent(item, event);
        delete event;
#else
        win->sendEvent(item, event);
        delete event;
#endif
    } else {
        // No window attached — clean up the event.
        delete event;
    }
}
#endif

bool EventInjector::dispatchEvent(QObject* target, QEvent* event)
{
    if (!target)
        return false;

    if (auto* w = qobject_cast<QWidget*>(target)) {
        // Real mouse events land on the deepest child under the cursor,
        // which for item views is the viewport; QAbstractItemView's own
        // filter forwards viewport events to the view handlers.  Target
        // the viewport so item views (combo popups, tables, trees, ...)
        // react to clicks the way they do under a real cursor.
        const int t = event->type();
        if (t >= QEvent::MouseButtonPress && t <= QEvent::MouseMove) {
            if (auto* iv = qobject_cast<QAbstractItemView*>(w)) {
                if (QWidget* vp = iv->viewport())
                    w = vp;
            }
        }
        sendToWidget(w, event);
        return true;
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        // QQuickWindow::sendEvent delivers the event straight to the given
        // item, bypassing the scene graph's hit testing.  Element clicks
        // therefore target the exact item the caller found (typically the
        // MouseArea itself); for coordinate/region clicks use mouseClickAt
        // / mouseClickRegion, which go through the real QPA input pipeline
        // with real hit testing.
        sendToQmlItem(item, event);
        return true;
    }
#endif

    if (auto* win = qobject_cast<QWindow*>(target)) {
        // Fallback: post to QWindow.
        QCoreApplication::postEvent(win, event);
        return true;
    }

    delete event;
    return false;
}

// ============================================================================
// Touch device
// ============================================================================

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
QPointingDevice* EventInjector::getTouchDevice()
{
    if (!s_touchDevice) {
        // QPointingDevice replaced QTouchDevice in Qt6; the type/capability
        // enums live on the QInputDevice base class.
        s_touchDevice = new QPointingDevice();
        s_touchDevice->setType(QInputDevice::DeviceType::TouchScreen);
        s_touchDevice->setCapabilities(QInputDevice::Capability::Position);
    }
    return s_touchDevice;
}
#else
QTouchDevice* EventInjector::getTouchDevice()
{
    if (!s_touchDevice) {
        s_touchDevice = new QTouchDevice();
        s_touchDevice->setType(QTouchDevice::TouchScreen);
        s_touchDevice->setCapabilities(QTouchDevice::Position);
    }
    return s_touchDevice;
}
#endif

// ============================================================================
// Mouse operations
// ============================================================================

bool EventInjector::mouseClick(QObject* target, const QString& button,
                                double x, double y,
                                const QStringList& modifiers, bool hasCoords)
{
    // Resolve coordinates once -- move, press and release must agree.
    getEventPos(target, x, y, hasCoords);
    // A real click always starts with the pointer moving to the target.
    // Some widgets depend on that: a combo popup's container sets the
    // view's currentIndex from MouseMove and only selects the item on
    // MouseButtonRelease, so a press+release without a preceding move
    // never selects anything.
    if (!mouseMove(target, x, y))
        return false;
    if (!mousePress(target, button, x, y, modifiers, hasCoords))
        return false;
    return mouseRelease(target, button, x, y, modifiers, hasCoords);
}

// ---------------------------------------------------------------------------
// mouseClickAt / mouseClickRegion  --  real QPA input pipeline
//
// Unlike the element-based ops above (which post events straight to the
// element via QCoreApplication/QQuickWindow::sendEvent), these route the
// click through QWindowSystemInterface::handleMouseEvent -- the entry point
// the platform plugins use for OS mouse messages.  The GUI thread then
// delivers the events with real hit testing: the scene graph for QML
// windows, the widget tree for widget windows.  Behavior is identical to a
// real mouse click at the given position.
// ---------------------------------------------------------------------------

bool EventInjector::mouseClickAt(QWindow* window, double x, double y,
                                 const QString& button,
                                 const QStringList& modifiers)
{
    if (!window)
        return false;

    const QPointF local(x, y);
    const QPointF global(window->mapToGlobal(local.toPoint()));
    const Qt::MouseButton btn = parseButton(button);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    // A real click sequence: move the pointer, press, release.  The events
    // are queued (thread-safe) and delivered on the GUI thread; for the
    // synchronous test path QWindowSystemInterface::sendWindowSystemEvents
    // flushes the queue.
    QWindowSystemInterface::handleMouseEvent(
        window, local, global, Qt::NoButton, Qt::NoButton,
        QEvent::MouseMove, mods);
    QWindowSystemInterface::handleMouseEvent(
        window, local, global, btn, btn,
        QEvent::MouseButtonPress, mods);
    QWindowSystemInterface::handleMouseEvent(
        window, local, global, Qt::NoButton, btn,
        QEvent::MouseButtonRelease, mods);
    return true;
}

bool EventInjector::mouseClickRegion(QObject* element, const QString& button,
                                     const QStringList& modifiers)
{
    if (!element)
        return false;

    if (auto* w = qobject_cast<QWidget*>(element)) {
        QWidget* win = w->window();
        if (!win || !win->windowHandle())
            return false;
        // Widget-local center -> top-level window-local coordinates.
        const QPoint local = w->mapTo(win, w->rect().center());
        return mouseClickAt(win->windowHandle(), local.x(), local.y(),
                            button, modifiers);
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(element)) {
        QQuickWindow* win = item->window();
        if (!win)
            return false;
        // mapToScene() yields window-local coordinates for a top-level
        // QQuickWindow (its contentItem spans the whole client area).
        const QPointF local = item->mapToScene(
            QPointF(item->width() / 2.0, item->height() / 2.0));
        return mouseClickAt(win, local.x(), local.y(), button, modifiers);
    }
#endif
    if (auto* win = qobject_cast<QWindow*>(element)) {
        return mouseClickAt(win, win->width() / 2.0, win->height() / 2.0,
                            button, modifiers);
    }
    return false;
}

QWindow* EventInjector::resolveWindow(QObject* elementOrWindow)
{
    if (!elementOrWindow)
        return nullptr;
    if (auto* w = qobject_cast<QWidget*>(elementOrWindow)) {
        QWidget* win = w->window();
        return win ? win->windowHandle() : nullptr;
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(elementOrWindow))
        return item->window();
#endif
    return qobject_cast<QWindow*>(elementOrWindow);
}

QWindow* EventInjector::primaryWindow()
{
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        for (QWidget* w : app->topLevelWidgets()) {
            if (w->isVisible() && w->windowHandle())
                return w->windowHandle();
        }
    }
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (w->isVisible())
            return w;
    }
    return nullptr;
}

bool EventInjector::mousePress(QObject* target, const QString& button,
                                double x, double y,
                                const QStringList& modifiers, bool hasCoords)
{
    getEventPos(target, x, y, hasCoords);
    const QPointF localPos(x, y);
    const Qt::MouseButton btn = parseButton(button);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    // Compute global position.
    QPoint globalPos(0, 0);
    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            QPointF scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

#ifdef QT_COMMANDER_QT6
    auto* event = new QMouseEvent(QEvent::MouseButtonPress,
                                   localPos, globalPos,
                                   btn, btn, mods);
#else
    auto* event = new QMouseEvent(QEvent::MouseButtonPress,
                                   localPos, localPos, globalPos,
                                   btn, btn, mods);
#endif

    s_pressedMouseButtons |= btn;  // so subsequent moves carry the button
    return dispatchEvent(target, event);
}

bool EventInjector::mouseRelease(QObject* target, const QString& button,
                                  double x, double y,
                                  const QStringList& modifiers, bool hasCoords)
{
    getEventPos(target, x, y, hasCoords);
    const QPointF localPos(x, y);
    const Qt::MouseButton btn = parseButton(button);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    QPoint globalPos(0, 0);
    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            QPointF scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

#ifdef QT_COMMANDER_QT6
    auto* event = new QMouseEvent(QEvent::MouseButtonRelease,
                                   localPos, globalPos,
                                   btn, Qt::NoButton, mods);
#else
    auto* event = new QMouseEvent(QEvent::MouseButtonRelease,
                                   localPos, localPos, globalPos,
                                   btn, Qt::NoButton, mods);
#endif

    s_pressedMouseButtons &= ~btn;  // drag finished; moves are hover-only
    return dispatchEvent(target, event);
}

bool EventInjector::mouseDblClick(QObject* target, const QString& button,
                                   double x, double y,
                                   const QStringList& modifiers, bool hasCoords)
{
    // A double-click is: press + release + dblclick + release.
    if (!mousePress(target, button, x, y, modifiers, hasCoords))
        return false;
    if (!mouseRelease(target, button, x, y, modifiers, hasCoords))
        return false;

    getEventPos(target, x, y, hasCoords);
    const QPointF localPos(x, y);
    const Qt::MouseButton btn = parseButton(button);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    QPoint globalPos(0, 0);
    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            QPointF scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

#ifdef QT_COMMANDER_QT6
    auto* dblEvent = new QMouseEvent(QEvent::MouseButtonDblClick,
                                      localPos, globalPos,
                                      btn, btn, mods);
#else
    auto* dblEvent = new QMouseEvent(QEvent::MouseButtonDblClick,
                                      localPos, localPos, globalPos,
                                      btn, btn, mods);
#endif

    dispatchEvent(target, dblEvent);

    // Final release (buttons = Qt::NoButton since the button is released).
#ifdef QT_COMMANDER_QT6
    auto* relEvent = new QMouseEvent(QEvent::MouseButtonRelease,
                                      localPos, globalPos,
                                      btn, Qt::NoButton, mods);
#else
    auto* relEvent = new QMouseEvent(QEvent::MouseButtonRelease,
                                      localPos, localPos, globalPos,
                                      btn, Qt::NoButton, mods);
#endif

    return dispatchEvent(target, relEvent);
}

bool EventInjector::mouseMove(QObject* target, double x, double y)
{
    const QPointF localPos(x, y);

    QPoint globalPos(0, 0);
    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            QPointF scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

#ifdef QT_COMMANDER_QT6
    auto* event = new QMouseEvent(QEvent::MouseMove,
                                   localPos, globalPos,
                                   Qt::NoButton, s_pressedMouseButtons,
                                   Qt::NoModifier);
#else
    auto* event = new QMouseEvent(QEvent::MouseMove,
                                   localPos, localPos, globalPos,
                                   Qt::NoButton, s_pressedMouseButtons,
                                   Qt::NoModifier);
#endif

    return dispatchEvent(target, event);
}

bool EventInjector::mouseWheel(QObject* target,
                                double deltaX, double deltaY,
                                double x, double y,
                                bool pixelDelta, bool hasCoords)
{
    getEventPos(target, x, y, hasCoords);
    const QPointF localPos(x, y);

    QPoint globalPos(0, 0);
    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            QPointF scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

    const QPoint angleDelta(qRound(deltaX * 120.0), qRound(deltaY * 120.0));
    const QPoint pixDelta(qRound(pixelDelta ? deltaX : 0.0),
                          qRound(pixelDelta ? deltaY : 0.0));

    QPoint gpos = globalPos;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6 removed the orientation parameter; phase/inverted have no defaults.
    auto* event = new QWheelEvent(QPointF(localPos), QPointF(gpos),
                                   pixDelta, angleDelta,
                                   Qt::NoButton, Qt::NoModifier,
                                   Qt::NoScrollPhase, false);
#else
    auto* event = new QWheelEvent(QPointF(localPos), QPointF(gpos),
                                   pixDelta, angleDelta,
                                   0, Qt::Vertical,
                                   Qt::NoButton, Qt::NoModifier);
#endif

    return dispatchEvent(target, event);
}

// ============================================================================
// Keyboard operations
// ============================================================================

bool EventInjector::keyPress(QObject* target,
                              const QString& key,
                              const QStringList& modifiers,
                              const QString& text)
{
    const int qtKey = parseKey(key);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    auto* event = new QKeyEvent(QEvent::KeyPress, qtKey, mods, text);
    return dispatchEvent(target, event);
}

bool EventInjector::keyRelease(QObject* target,
                                const QString& key,
                                const QStringList& modifiers,
                                const QString& text)
{
    const int qtKey = parseKey(key);
    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    auto* event = new QKeyEvent(QEvent::KeyRelease, qtKey, mods, text);
    return dispatchEvent(target, event);
}

bool EventInjector::typeText(QObject* target,
                              const QString& text,
                              int intervalMs,
                              const QStringList& modifiers)
{
    if (text.isEmpty())
        return true;

    const Qt::KeyboardModifiers mods = parseModifiers(modifiers);

    // For typeText we use synchronous dispatch to respect the interval.
    // Determine the target type.
    auto* widget = qobject_cast<QWidget*>(target);
#ifdef QT_COMMANDER_WITH_QML
    auto* item   = qobject_cast<QQuickItem*>(target);
#endif
    auto* win    = qobject_cast<QWindow*>(target);

#ifdef QT_COMMANDER_WITH_QML
    if (!widget && !item && !win)
        return false;
#else
    if (!widget && !win)
        return false;
#endif

    for (const QChar& ch : text) {
        const int qtKey = parseKey(QString(ch));
        const QString chStr(ch);

        auto* pressEvent = new QKeyEvent(QEvent::KeyPress,
                                          qtKey,
                                          mods,
                                          chStr);
        auto* releaseEvent = new QKeyEvent(QEvent::KeyRelease,
                                            qtKey,
                                            mods,
                                            chStr);

        if (widget) {
            QCoreApplication::postEvent(widget, pressEvent);
            QCoreApplication::postEvent(widget, releaseEvent);
#ifdef QT_COMMANDER_WITH_QML
        } else if (item) {
            QQuickWindow* qmlWin = item->window();
            if (qmlWin) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                // Qt6 removed QQuickWindow::sendEvent -- deliver directly
                // to the item (sendEvent does not take ownership).
                QCoreApplication::sendEvent(item, pressEvent);
                QCoreApplication::sendEvent(item, releaseEvent);
                delete pressEvent;
                delete releaseEvent;
#else
                qmlWin->sendEvent(item, pressEvent);
                qmlWin->sendEvent(item, releaseEvent);
                delete pressEvent;
                delete releaseEvent;
#endif
            } else {
                delete pressEvent;
                delete releaseEvent;
            }
#endif
        } else if (win) {
            QCoreApplication::postEvent(win, pressEvent);
            QCoreApplication::postEvent(win, releaseEvent);
        }

        if (intervalMs > 0)
            QThread::msleep(intervalMs);
    }
    return true;
}

bool EventInjector::keyCombo(QObject* target, const QString& keys)
{
    // Format: "Ctrl+Shift+A" (last part is the key, rest are modifiers).
    QStringList parts = keys.split(QLatin1Char('+'));
    if (parts.isEmpty())
        return false;

    const QString keyName = parts.takeLast();

    // Convert modifier names to lowercase for parsing.
    QStringList modNames;
    for (const QString& p : parts)
        modNames.append(p.toLower());

    const int qtKey = parseKey(keyName);
    const Qt::KeyboardModifiers mods = parseModifiers(modNames);

    auto* widget = qobject_cast<QWidget*>(target);
#ifdef QT_COMMANDER_WITH_QML
    auto* item   = qobject_cast<QQuickItem*>(target);
#endif
    auto* win    = qobject_cast<QWindow*>(target);

#ifdef QT_COMMANDER_WITH_QML
    if (!widget && !item && !win)
        return false;
#else
    if (!widget && !win)
        return false;
#endif

    // Helper lambdas for dispatch.
    auto doPress = [&](QEvent* e) {
        if (widget) QCoreApplication::postEvent(widget, e);
#ifdef QT_COMMANDER_WITH_QML
        else if (item) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (auto* w = item->window()) {
                QCoreApplication::sendEvent(item, e);
                delete e;
            } else delete e;
#else
            if (auto* w = item->window()) { w->sendEvent(item, e); delete e; }
            else delete e;
#endif
        }
#endif
        else if (win) QCoreApplication::postEvent(win, e);
    };
    auto doRelease = [&](QEvent* e) {
        if (widget) QCoreApplication::postEvent(widget, e);
#ifdef QT_COMMANDER_WITH_QML
        else if (item) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (auto* w = item->window()) {
                QCoreApplication::sendEvent(item, e);
                delete e;
            } else delete e;
#else
            if (auto* w = item->window()) { w->sendEvent(item, e); delete e; }
            else delete e;
#endif
        }
#endif
        else if (win) QCoreApplication::postEvent(win, e);
    };

    // Press modifiers one by one.
    for (const QString& mn : modNames) {
        int modKey = Qt::Key_unknown;
        if (mn == QStringLiteral("shift"))
            modKey = Qt::Key_Shift;
        else if (mn == QStringLiteral("ctrl") || mn == QStringLiteral("control"))
            modKey = Qt::Key_Control;
        else if (mn == QStringLiteral("alt"))
            modKey = Qt::Key_Alt;
        else if (mn == QStringLiteral("meta"))
            modKey = Qt::Key_Meta;

        if (modKey != Qt::Key_unknown)
            doPress(new QKeyEvent(QEvent::KeyPress, modKey, Qt::NoModifier, QString()));
    }

    // Press main key with all modifiers.
    doPress(new QKeyEvent(QEvent::KeyPress, qtKey, mods, keyName));

    // Release main key.
    doRelease(new QKeyEvent(QEvent::KeyRelease, qtKey, mods, keyName));

    // Release modifiers in reverse order.
    for (int i = modNames.size() - 1; i >= 0; --i) {
        int modKey = Qt::Key_unknown;
        const QString& mn = modNames[i];
        if (mn == QStringLiteral("shift"))
            modKey = Qt::Key_Shift;
        else if (mn == QStringLiteral("ctrl") || mn == QStringLiteral("control"))
            modKey = Qt::Key_Control;
        else if (mn == QStringLiteral("alt"))
            modKey = Qt::Key_Alt;
        else if (mn == QStringLiteral("meta"))
            modKey = Qt::Key_Meta;

        if (modKey != Qt::Key_unknown)
            doRelease(new QKeyEvent(QEvent::KeyRelease, modKey, Qt::NoModifier, QString()));
    }

    return true;
}

// ============================================================================
// Touch operations
// ============================================================================

bool EventInjector::touchPress(QObject* target,
                                double x, double y,
                                int touchId, double pressure)
{
    if (!target)
        return false;

    // Save target for later release.
    s_touchTargets[touchId] = {target};

    const QPointF localPos(x, y);

    // Compute global + scene positions.
    QPointF scenePos = localPos;
    QPoint globalPos(0, 0);

    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
        scenePos = localPos;
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

    auto* dev = getTouchDevice();

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    QEventPoint tp(touchId, QEventPoint::State(Qt::TouchPointPressed),
                   scenePos, globalPos);
    QMutableEventPoint::setPosition(tp, localPos);
    QMutableEventPoint::setPressure(tp, pressure);
    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);
    auto* event = new QTouchEvent(QEvent::TouchBegin,
                                   dev,
                                   Qt::NoModifier,
                                   touchPoints);
#else
    QTouchEvent::TouchPoint tp;
    tp.setId(touchId);
    tp.setState(Qt::TouchPointPressed);
    tp.setPos(localPos);
    tp.setScenePos(scenePos);
    tp.setScreenPos(globalPos);
    tp.setPressure(pressure);

    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);

    auto* event = new QTouchEvent(QEvent::TouchBegin,
                                   dev,
                                   Qt::NoModifier,
                                   Qt::TouchPointPressed,
                                   touchPoints);
#endif

    return dispatchEvent(target, event);
}

bool EventInjector::touchMove(QObject* target,
                               double x, double y,
                               int touchId, double pressure)
{
    if (!target)
        return false;

    const QPointF localPos(x, y);

    QPointF scenePos = localPos;
    QPoint globalPos(0, 0);

    if (auto* w = qobject_cast<QWidget*>(target)) {
        globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
    }
#ifdef QT_COMMANDER_WITH_QML
    else if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (win) {
            scenePos = item->mapToScene(localPos);
            globalPos = win->mapToGlobal(scenePos.toPoint());
        }
    }
#endif

    auto* dev = getTouchDevice();

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    QEventPoint tp(touchId, QEventPoint::State(Qt::TouchPointMoved),
                   scenePos, globalPos);
    QMutableEventPoint::setPosition(tp, localPos);
    QMutableEventPoint::setPressure(tp, pressure);
    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);
    auto* event = new QTouchEvent(QEvent::TouchUpdate,
                                   dev,
                                   Qt::NoModifier,
                                   touchPoints);
#else
    QTouchEvent::TouchPoint tp;
    tp.setId(touchId);
    tp.setState(Qt::TouchPointMoved);
    tp.setPos(localPos);
    tp.setScenePos(scenePos);
    tp.setScreenPos(globalPos);
    tp.setPressure(pressure);

    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);

    auto* event = new QTouchEvent(QEvent::TouchUpdate,
                                   dev,
                                   Qt::NoModifier,
                                   Qt::TouchPointMoved,
                                   touchPoints);
#endif

    return dispatchEvent(target, event);
}

bool EventInjector::touchRelease(int touchId)
{
    // Look up the target from the press tracking.
    auto it = s_touchTargets.find(touchId);
    if (it == s_touchTargets.end())
        return false;

    QObject* target = it.value().obj;
    s_touchTargets.erase(it);

    if (!target)
        return false;

    // For release, the position doesn't matter as much.
    const QPointF localPos(0, 0);
    auto* dev = getTouchDevice();

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    QEventPoint tp(touchId, QEventPoint::State(Qt::TouchPointReleased),
                   localPos, QPointF(0, 0));
    QMutableEventPoint::setPosition(tp, localPos);
    QMutableEventPoint::setPressure(tp, 0);
    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);
    auto* event = new QTouchEvent(QEvent::TouchEnd,
                                   dev,
                                   Qt::NoModifier,
                                   touchPoints);
#else
    QTouchEvent::TouchPoint tp;
    tp.setId(touchId);
    tp.setState(Qt::TouchPointReleased);
    tp.setPos(localPos);
    tp.setScenePos(localPos);
    tp.setScreenPos(QPoint(0, 0));
    tp.setPressure(0);

    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.append(tp);

    auto* event = new QTouchEvent(QEvent::TouchEnd,
                                   dev,
                                   Qt::NoModifier,
                                   Qt::TouchPointReleased,
                                   touchPoints);
#endif

    return dispatchEvent(target, event);
}

// ============================================================================
// Focus operations
// ============================================================================

bool EventInjector::focus(QObject* target)
{
    if (auto* w = qobject_cast<QWidget*>(target)) {
        // SetFocus() only updates QApplication::focusWidget() while the
        // window is considered active.  In an injected context the OS
        // foreground window is usually another application, so force the
        // Qt-side activation explicitly (that is what focusWidget() reads),
        // then attempt the OS-level foreground switch as well.
        if (QWidget* win = w->window()) {
            QApplication::setActiveWindow(win);
            win->activateWindow();
            win->raise();
#ifdef Q_OS_WIN
            HWND hwnd = reinterpret_cast<HWND>(win->winId());
            if (hwnd) {
                HWND fg = GetForegroundWindow();
                if (fg != hwnd) {
                    DWORD curThread = GetCurrentThreadId();
                    DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
                    if (curThread != fgThread) {
                        AttachThreadInput(curThread, fgThread, TRUE);
                        SetForegroundWindow(hwnd);
                        AttachThreadInput(curThread, fgThread, FALSE);
                    } else {
                        SetForegroundWindow(hwnd);
                    }
                }
                SetFocus(hwnd);
            }
#endif
        }
        w->setFocus();
        return true;
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        item->forceActiveFocus();
        return true;
    }
#endif
    return false;
}

bool EventInjector::clearFocus(QObject* target)
{
    if (auto* w = qobject_cast<QWidget*>(target)) {
        w->clearFocus();
        return true;
    }
#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        item->setFocus(false);
        return true;
    }
#endif
    return false;
}

// ============================================================================
// Context menu
// ============================================================================

bool EventInjector::contextMenu(QObject* target,
                                 double x, double y,
                                 bool hasCoords)
{
    getEventPos(target, x, y, hasCoords);

    if (auto* w = qobject_cast<QWidget*>(target)) {
        QPoint globalPos = w->mapToGlobal(QPoint(qRound(x), qRound(y)));
        auto* event = new QContextMenuEvent(QContextMenuEvent::Mouse,
                                             QPoint(qRound(x), qRound(y)),
                                             globalPos,
                                             Qt::NoModifier);
        QCoreApplication::postEvent(w, event);
        return true;
    }

#ifdef QT_COMMANDER_WITH_QML
    if (auto* item = qobject_cast<QQuickItem*>(target)) {
        QQuickWindow* win = item->window();
        if (!win)
            return false;

        // Quick items handle context menu via MouseArea onPressAndHold
        // or via right-click mouse events.  Send a right-click press+release
        // which is how Qt Quick typically triggers context menus.
        const QPointF localPos(x, y);
        QPointF scenePos = item->mapToScene(localPos);
        QPoint globalPos = win->mapToGlobal(scenePos.toPoint());

#ifdef QT_COMMANDER_QT6
        auto* pressEv = new QMouseEvent(QEvent::MouseButtonPress,
                                         localPos, globalPos,
                                         Qt::RightButton, Qt::RightButton,
                                         Qt::NoModifier);
        auto* releaseEv = new QMouseEvent(QEvent::MouseButtonRelease,
                                           localPos, globalPos,
                                           Qt::RightButton, Qt::NoButton,
                                           Qt::NoModifier);
#else
        auto* pressEv = new QMouseEvent(QEvent::MouseButtonPress,
                                         localPos, localPos, globalPos,
                                         Qt::RightButton, Qt::RightButton,
                                         Qt::NoModifier);
        auto* releaseEv = new QMouseEvent(QEvent::MouseButtonRelease,
                                           localPos, localPos, globalPos,
                                           Qt::RightButton, Qt::NoButton,
                                           Qt::NoModifier);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QCoreApplication::sendEvent(item, pressEv);
        QCoreApplication::sendEvent(item, releaseEv);
#else
        win->sendEvent(item, pressEv);
        win->sendEvent(item, releaseEv);
#endif
        delete pressEv;
        delete releaseEv;
        return true;
    }
#endif

    return false;
}
