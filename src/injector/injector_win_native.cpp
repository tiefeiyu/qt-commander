#include "injector.h"

#include <windows.h>

#include <string>

namespace {

std::string lastError()
{
    const DWORD code = GetLastError();
    return "Win32 error " + std::to_string(code);
}

} // namespace

InjectResult injectLibrary(int pid, const fs::path& lib_path)
{
    const std::wstring path = fs::absolute(lib_path).wstring();
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, DWORD(pid));
    if (!process)
        return {false, "OpenProcess failed: " + lastError()};

    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return {false, "VirtualAllocEx failed: " + lastError()};
    }
    if (!WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return {false, "WriteProcessMemory failed: " + lastError()};
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return {false, "LoadLibraryW not found"};
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote,
                                       0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return {false, "CreateRemoteThread failed: " + lastError()};
    }
    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(thread);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return {false, "remote LoadLibrary timed out"};
    }
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (exitCode == 0)
        return {false, "target LoadLibraryW returned NULL"};
    return {true, {}};
}
