#include "qt_commander/agent.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::atomic<bool> g_started{false};

void startRuntime()
{
    if (g_started.exchange(true))
        return;
    qt_commander::StartOptions options;
    options.name = QStringLiteral("injected");
    options.mode = QStringLiteral("injection");
    const QByteArray rendezvous = qgetenv("QT_COMMANDER_RENDEZVOUS");
    if (!rendezvous.isEmpty())
        options.rendezvous_dir = QString::fromLocal8Bit(rendezvous);
    qt_commander::start(options);
}

// Runs on the GUI thread while QCoreApplication is being constructed.
//
// A library loaded *before* main — LD_PRELOAD on Linux, DYLD_INSERT_LIBRARIES
// on macOS — hits this path, so the runtime starts with no waiting and no
// timeout: there is nothing to wait for, the application object is right
// there being built.
void startupFunction()
{
    startRuntime();
}

// Fallback for a library loaded into an already-running process (the ptrace
// injector): the startup function has already fired and never will again, so
// poll for the application object and marshal onto its thread.
void pollBootstrap()
{
    std::thread([]() {
        // Give a preloaded library's startup function a chance to win first;
        // the preload case leaves this thread asleep and it exits immediately.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (int attempt = 0; attempt < 200 && !g_started.load(); ++attempt) {
            QCoreApplication* app = QCoreApplication::instance();
            if (app) {
                QMetaObject::invokeMethod(
                    app, []() { startRuntime(); },
                    Qt::BlockingQueuedConnection);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

} // namespace

// Q_COREAPP_STARTUP_FUNCTION expands to a function definition plus a static
// registrar, so it must sit at namespace scope — not inside a function.
// The registrar runs when this library is loaded (before main for a preloaded
// library), and the callback fires when QCoreApplication is constructed.
Q_COREAPP_STARTUP_FUNCTION(startupFunction)

// The constructor runs on the loader thread; do not touch QApplication here.
// Only arm the late-injection fallback.
__attribute__((constructor)) static void qtc_unix_bootstrap()
{
    pollBootstrap();
}
