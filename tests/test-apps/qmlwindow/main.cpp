#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QUrl>
#include <QDebug>
#include "qt_commander/agent.h"

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmlwindow-test"));
    QQmlApplicationEngine engine;
    engine.load(QUrl::fromLocalFile(QStringLiteral(QML_SOURCE)));
    if (engine.rootObjects().isEmpty())
        return 1;

    QQuickWindow hiddenWindow;
    hiddenWindow.setObjectName(QStringLiteral("hiddenWindow"));
    hiddenWindow.setTitle(QStringLiteral("Hidden Window"));
    hiddenWindow.resize(100, 80);

    qt_commander::StartOptions options;
    options.name = QStringLiteral("qmlwindow");
    options.rendezvous_dir = qEnvironmentVariable("QT_COMMANDER_RENDEZVOUS");
    const auto result = qt_commander::start(options);
    if (!result.ok)
        qWarning() << "qt-commander start failed:" << result.error;
    return app.exec();
}
