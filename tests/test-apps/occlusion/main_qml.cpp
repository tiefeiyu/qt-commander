// QML occlusion test app for qt-commander integration tests.
// Loads occlusion_scene.qml from the qrc resource (no filesystem
// dependency).  QQmlApplicationEngine is required: the scene root is a
// Window, which QQuickView explicitly refuses to load.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QFile>
#include <QDateTime>
#include <QTextStream>
#include <QtGlobal>

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC-only: UCRT abort-behavior tweaks; crtdbg/_set_abort_behavior are
// not available on MinGW's msvcrt.
#include <windows.h>
#include <crtdbg.h>
#endif

// This is a WIN32_EXECUTABLE (no console); route Qt messages to a log file
// next to the executable so QML load errors are visible.
static void logMessageHandler(QtMsgType type, const QMessageLogContext&,
                              const QString& msg)
{
    static QFile* log = []() {
        QFile* f = new QFile(QCoreApplication::applicationDirPath()
                             + QStringLiteral("/qt-occlusion-qml-test.log"));
        f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return f;
    }();
    QTextStream ts(log);
    ts << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
       << ' ' << msg << '\n';
    ts.flush();
}

int main(int argc, char* argv[])
{
#if defined(_WIN32) && defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(logMessageHandler);

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/occlusion_scene.qml")));

    return app.exec();
}
