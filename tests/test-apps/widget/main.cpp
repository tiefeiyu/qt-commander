// QWidget-only test application for qt-commander integration tests.
// No QML or QQuickWidget dependency — safe to build with WITH_QML=OFF.
#include "mainwindow.h"
#include <QApplication>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QApplication app(argc, argv);

    WidgetTestWindow win;
    win.show();

    return app.exec();
}
