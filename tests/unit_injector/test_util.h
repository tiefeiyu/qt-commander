// Test utilities shared by injector unit tests.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Change the working directory to the directory containing this test
// executable.  Binary discovery in the E2E tests relies on "." / ".."
// relative paths: ctest runs from the build dir, but a direct launch
// (e.g. from the repo root) must not resolve against the caller's CWD --
// otherwise a sibling older build tree ("build/msvc") can win over the
// tree this exe came from and the test injects version-mismatched
// binaries.  Anchoring CWD to the exe dir makes both cases identical.
static inline void chdir_to_exe_dir() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
        wchar_t* sep = wcsrchr(path, L'\\');
        if (sep) *sep = L'\0';
        SetCurrentDirectoryW(path);
    }
}
