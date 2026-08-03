// QML test app for qt-commander integration tests.
// Loads the scene from the qrc resource (no filesystem dependency), so the
// app runs from any working directory and after windeployqt deployment.
// QQmlApplicationEngine is required: the scene root is a Window, which
// QQuickView explicitly refuses to load.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QFile>
#include <QDateTime>
#include <QDebug>

#ifdef _WIN32
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
                             + QStringLiteral("/qt-qml-test.log"));
        f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return f;
    }();
    QTextStream ts(log);
    ts << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
       << ' ' << msg << '\n';
    ts.flush();
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(logMessageHandler);

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/test_scene.qml")));

    return app.exec();
}
