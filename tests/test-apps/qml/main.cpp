// Pure-QML test app using QQuickView (creates a definite QQuickWindow)
#include <QGuiApplication>
#include <QQuickView>
#include <QQmlEngine>
#include <QQmlContext>
#include <QUrl>
#include <QFileInfo>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QGuiApplication app(argc, argv);

    QQuickView view;
    view.setTitle("QML Test App");
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(800, 600);

    // Load QML from the same directory as the executable
    QString qmlPath = QFileInfo(app.applicationFilePath()).absolutePath() + "/test_scene.qml";
    view.setSource(QUrl::fromLocalFile(qmlPath));
    view.show();

    return app.exec();
}
