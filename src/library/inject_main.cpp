#include "qt_commander/agent.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QByteArray>
#include <QString>

#include <cstring>
#include <cwchar>

#include <windows.h>

namespace {

void writeStatus(const char* message)
{
    char directory[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, directory))
        return;
    char path[MAX_PATH] = {};
    lstrcpyA(path, directory);
    char name[64] = {};
    wsprintfA(name, "qtc-inject-status-%lu.txt", GetCurrentProcessId());
    lstrcatA(path, name);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written,
              nullptr);
    CloseHandle(file);
}

void logLine(const char* message)
{
    if (qgetenv("QT_COMMANDER_INJECT_DEBUG").isEmpty())
        return;
    wchar_t path[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, path))
        return;
    const wchar_t suffix[] = L"qtc-inject.log";
    if (wcslen(path) + sizeof(suffix) / sizeof(suffix[0]) >= MAX_PATH)
        return;
    wcscat_s(path, MAX_PATH, suffix);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written,
              nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

DWORD WINAPI bootstrap(LPVOID)
{
    // Let LoadLibraryW finish and release the loader lock before touching Qt.
    Sleep(500);
    logLine("bootstrap thread started");
    // The injector may attach before QApplication exists. Wait for the GUI
    // thread's application object, then run start() on that thread.
    for (int attempt = 0; attempt < 200; ++attempt) {
        QCoreApplication* app = QCoreApplication::instance();
        if (app) {
            logLine("QCoreApplication found");
            qt_commander::StartResult result;
            QMetaObject::invokeMethod(
                app,
                [&result]() {
                    qt_commander::StartOptions options;
                    options.name = QStringLiteral("injected");
                    options.mode = QStringLiteral("injection");
                    const QByteArray rendezvous =
                        qgetenv("QT_COMMANDER_RENDEZVOUS");
                    if (!rendezvous.isEmpty())
                        options.rendezvous_dir =
                            QString::fromLocal8Bit(rendezvous);
                    result = qt_commander::start(options);
                },
                Qt::BlockingQueuedConnection);
            logLine(result.ok ? "start ok" : "start failed");
            writeStatus(result.ok ? "ok" : result.error.toUtf8().constData());
            if (!result.ok)
                logLine(result.error.toUtf8().constData());
            return 0;
        }
        Sleep(50);
    }
    writeStatus("timeout waiting for QCoreApplication");
    logLine("no QCoreApplication after 10s");
    return 1;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
