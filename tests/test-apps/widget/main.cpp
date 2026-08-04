// QWidget-only test application for qt-commander integration tests.
// No QML or QQuickWidget dependency — safe to build with WITH_QML=OFF.
#include "mainwindow.h"
#include <QApplication>

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC-only: UCRT abort-behavior tweaks; crtdbg/_set_abort_behavior are
// not available on MinGW's msvcrt.
#include <windows.h>
#include <crtdbg.h>
#endif

int main(int argc, char* argv[])
{
#if defined(_WIN32) && defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QApplication app(argc, argv);

    WidgetTestWindow win;
    win.show();

    return app.exec();
}
