#include "qt_commander/agent.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include <chrono>
#include <thread>

namespace {

void bootstrap()
{
    std::thread([]() {
        for (int attempt = 0; attempt < 200; ++attempt) {
            QCoreApplication* app = QCoreApplication::instance();
            if (app) {
                QMetaObject::invokeMethod(
                    app,
                    []() {
                        qt_commander::StartOptions options;
                        options.name = QStringLiteral("injected");
                        options.mode = QStringLiteral("injection");
                        const QByteArray rendezvous =
                            qgetenv("QT_COMMANDER_RENDEZVOUS");
                        if (!rendezvous.isEmpty())
                            options.rendezvous_dir =
                                QString::fromLocal8Bit(rendezvous);
                        qt_commander::start(options);
                    },
                    Qt::BlockingQueuedConnection);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

} // namespace

__attribute__((constructor)) static void qtc_linux_bootstrap()
{
    // The constructor runs on the loader thread; do not touch QApplication
    // here. The detached thread waits for the target's QCoreApplication.
    bootstrap();
}
