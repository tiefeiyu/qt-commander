// Handler test stubs
//
// Minimal implementations of UiScanner, EventInjector, and Screenshot
// functions that handler.cpp references.  These stubs allow linking the
// test executable without compiling the full library.
//
// None of these stubs are expected to be called during the actual handler
// tests -- they exist only to satisfy the linker.

#include "core/ui_scanner.h"
#include "core/event_injector.h"
#include "core/screenshot.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>

// ============================================================================
// UiScanner stubs
// ============================================================================

/* static */
QJsonObject UiScanner::generateSnapshot(
    const QString& /*session_id*/,
    int /*snapshot_id*/,
    const QString& /*detail*/,
    bool /*include_hidden*/,
    ElementMap* /*element_map*/,
    const QString& /*snapshot_dir*/)
{
    return QJsonObject();
}

// ============================================================================
// EventInjector stubs
// ============================================================================

/* static */ QHash<int, EventInjector::TouchTarget> EventInjector::s_touchTargets;

/* static */
bool EventInjector::mouseClick(QObject* /*target*/, const QString& /*button*/,
                                double /*x*/, double /*y*/,
                                const QStringList& /*modifiers*/, bool /*hasCoords*/)
{ return true; }

/* static */
bool EventInjector::mousePress(QObject* /*target*/, const QString& /*button*/,
                                double /*x*/, double /*y*/,
                                const QStringList& /*modifiers*/, bool /*hasCoords*/)
{ return true; }

/* static */
bool EventInjector::mouseRelease(QObject* /*target*/, const QString& /*button*/,
                                  double /*x*/, double /*y*/,
                                  const QStringList& /*modifiers*/, bool /*hasCoords*/)
{ return true; }

/* static */
bool EventInjector::mouseDblClick(QObject* /*target*/, const QString& /*button*/,
                                   double /*x*/, double /*y*/,
                                   const QStringList& /*modifiers*/, bool /*hasCoords*/)
{ return true; }

/* static */
bool EventInjector::mouseMove(QObject* /*target*/, double /*x*/, double /*y*/)
{ return true; }

/* static */
bool EventInjector::mouseWheel(QObject* /*target*/,
                                double /*deltaX*/, double /*deltaY*/,
                                double /*x*/, double /*y*/,
                                bool /*pixelDelta*/, bool /*hasCoords*/)
{ return true; }

/* static */
bool EventInjector::keyPress(QObject* /*target*/, const QString& /*key*/,
                              const QStringList& /*modifiers*/, const QString& /*text*/)
{ return true; }

/* static */
bool EventInjector::keyRelease(QObject* /*target*/, const QString& /*key*/,
                                const QStringList& /*modifiers*/, const QString& /*text*/)
{ return true; }

/* static */
bool EventInjector::typeText(QObject* /*target*/, const QString& /*text*/, int /*intervalMs*/)
{ return true; }

/* static */
bool EventInjector::keyCombo(QObject* /*target*/, const QString& /*keys*/)
{ return true; }

/* static */
bool EventInjector::touchPress(QObject* /*target*/, double /*x*/, double /*y*/,
                                int /*touchId*/, double /*pressure*/)
{ return true; }

/* static */
bool EventInjector::touchMove(QObject* /*target*/, double /*x*/, double /*y*/,
                               int /*touchId*/, double /*pressure*/)
{ return true; }

/* static */
bool EventInjector::touchRelease(int /*touchId*/)
{ return true; }

/* static */
bool EventInjector::focus(QObject* /*target*/)
{ return true; }

/* static */
bool EventInjector::clearFocus(QObject* /*target*/)
{ return true; }

/* static */
bool EventInjector::contextMenu(QObject* /*target*/, double /*x*/, double /*y*/,
                                 bool /*hasCoords*/)
{ return true; }

// ============================================================================
// Screenshot stubs
// ============================================================================

/* static */
QString Screenshot::capture(QObject* /*target*/, const QString& /*screenshot_dir*/,
                             int /*sequence*/)
{ return QString(); }
