"""Windows DLL injector used by the injection attach mode.

The injector only loads a Qt-commander bootstrap DLL.  The DLL itself waits for
the target's QCoreApplication and starts the same in-process RPC server as
linked mode, so protocol and tool behavior stay identical.
"""
from __future__ import annotations

import ctypes
import os
from pathlib import Path

from .errors import ErrorCode

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x00001000
MEM_RESERVE = 0x00002000
MEM_RELEASE = 0x00008000
PAGE_READWRITE = 0x04
WAIT_OBJECT_0 = 0x00000000
INFINITE = 0xFFFFFFFF


class InjectionError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def default_dll_path() -> Path | None:
    override = os.environ.get("QT_COMMANDER_INJECT_DLL")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    root = Path(__file__).resolve().parents[1]
    candidates = {
        path.resolve()
        for pattern in ("build/*/qt-commander/qt_commander_inject.dll",
                        "build/*/qt_commander_inject.dll")
        for path in root.glob(pattern)
    }
    if not candidates:
        return None
    try:
        return max(candidates, key=lambda path: path.stat().st_mtime)
    except OSError:
        return None


def inject_library(pid: int, dll_path: Path, *, timeout_ms: int = 8000) -> None:
    if os.name != "nt":
        raise InjectionError(
            ErrorCode.INJECTION_BLOCKED.value,
            "DLL injection is only implemented on Windows",
        )
    dll_path = dll_path.resolve()
    if not dll_path.is_file():
        raise InjectionError(
            ErrorCode.INJECTION_FAILED.value,
            f"injection DLL not found: {dll_path}",
        )

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int,
                                     ctypes.c_uint32]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.c_uint32,
                                        ctypes.c_uint32]
    kernel32.VirtualAllocEx.restype = ctypes.c_void_p
    kernel32.VirtualFreeEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.c_uint32]
    kernel32.WriteProcessMemory.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.CreateRemoteThread.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
    ]
    kernel32.CreateRemoteThread.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.GetExitCodeThread.argtypes = [ctypes.c_void_p,
                                           ctypes.POINTER(ctypes.c_uint32)]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    process = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not process:
        raise InjectionError(
            ErrorCode.PERMISSION.value,
            f"OpenProcess failed (pid {pid}, error {ctypes.get_last_error()})",
        )

    remote_path = None
    thread = None
    thread_completed = False
    try:
        encoded = str(dll_path).encode("utf-16-le") + b"\x00\x00"
        remote_path = kernel32.VirtualAllocEx(
            process, None, len(encoded), MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE,
        )
        if not remote_path:
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                f"VirtualAllocEx failed (error {ctypes.get_last_error()})",
            )
        written = ctypes.c_size_t()
        buffer = ctypes.create_string_buffer(encoded, len(encoded))
        if not kernel32.WriteProcessMemory(
            process, remote_path, buffer, len(encoded), ctypes.byref(written)
        ):
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                f"WriteProcessMemory failed (error {ctypes.get_last_error()})",
            )

        load_library = kernel32.GetProcAddress(
            kernel32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW"
        )
        if not load_library:
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                "LoadLibraryW not found",
            )
        thread = kernel32.CreateRemoteThread(
            process, None, 0, load_library, remote_path, 0, None
        )
        if not thread:
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                f"CreateRemoteThread failed (error {ctypes.get_last_error()})",
            )
        wait = kernel32.WaitForSingleObject(thread, timeout_ms)
        if wait != WAIT_OBJECT_0:
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                "injected LoadLibrary did not finish",
            )
        thread_completed = True
        exit_code = ctypes.c_uint32()
        kernel32.GetExitCodeThread(thread, ctypes.byref(exit_code))
        if not exit_code.value:
            raise InjectionError(
                ErrorCode.INJECTION_FAILED.value,
                "target LoadLibraryW returned NULL (missing DLL dependency?)",
            )
    finally:
        if thread:
            kernel32.CloseHandle(thread)
        if remote_path and thread_completed:
            kernel32.VirtualFreeEx(process, remote_path, 0, MEM_RELEASE)
        kernel32.CloseHandle(process)
