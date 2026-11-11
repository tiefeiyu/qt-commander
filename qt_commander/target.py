"""Best-effort target-process architecture/Qt metadata.

This intentionally does not inject or execute anything in the target. It reads
the executable image and adjacent runtime files, and optionally a live
descriptor. Callers should treat the result as evidence, not a guarantee.
"""

from __future__ import annotations

import json
import os
import platform
import re
import struct
import time
from pathlib import Path
from typing import Any

import psutil

from . import artifacts

_MODULE_CACHE: dict[tuple[int, float], dict[str, Any]] = {}
_MODULE_SCAN_BUDGET_SECONDS = 2.0
_MODULE_SCAN_ENV = "QT_COMMANDER_PROCESS_MODULES"
_MODULE_EVIDENCE_LIMIT = 8
_SO_VERSION_RE = re.compile(r"\.so(?:\.(\d+(?:\.\d+)*))?$")

_ARCH_BY_PE_MACHINE = {
    0x014C: "x86",
    0x8664: "x64",
    0xAA64: "arm64",
    0xA641: "arm64",
    0x01C0: "arm",
    0x01C4: "arm",
}
_ARCH_BY_ELF_MACHINE = {
    3: "x86",
    62: "x64",
    183: "arm64",
    40: "arm",
}


def _read_descriptor(path: str | Path) -> dict[str, Any]:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _pe_info(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        header = stream.read(0x40)
        if len(header) < 0x40 or header[:2] != b"MZ":
            return {}
        offset = struct.unpack_from("<I", header, 0x3C)[0]
        stream.seek(offset)
        pe = stream.read(6)
    if len(pe) < 6 or pe[:4] != b"PE\0\0":
        return {}
    machine = struct.unpack_from("<H", pe, 4)[0]
    arch = _ARCH_BY_PE_MACHINE.get(machine)
    return {
        "format": "pe",
        "arch": arch or f"unknown(0x{machine:04x})",
        "bits": 64 if arch in {"x64", "arm64"} else 32,
    }


def _elf_info(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return {}
    elf_class = header[4]
    endian = "<" if header[5] == 1 else ">"
    machine = struct.unpack_from(f"{endian}H", header, 18)[0]
    arch = _ARCH_BY_ELF_MACHINE.get(machine)
    return {
        "format": "elf",
        "arch": arch or f"unknown({machine})",
        "bits": 64 if elf_class == 2 else 32,
    }


def inspect_executable(path: str | Path) -> dict[str, Any]:
    """Inspect a PE or ELF image without loading it."""
    file_path = Path(path)
    try:
        info = _pe_info(file_path) or _elf_info(file_path)
    except OSError as exc:
        return {"error": str(exc), "executable": str(file_path)}
    result: dict[str, Any] = {"executable": str(file_path)}
    result.update(info)
    result["confidence"] = "high" if info else "low"
    return result


def _descriptor_qt(data: dict[str, Any]) -> dict[str, Any]:
    qt = data.get("qt")
    if not isinstance(qt, dict):
        return {}
    result: dict[str, Any] = {}
    version = qt.get("version")
    if isinstance(version, str) and version.strip():
        result["qt_version"] = version.strip()
    major = qt.get("major")
    if isinstance(major, int):
        result["qt_major"] = major
    return result


def _file_version(path: Path) -> str | None:
    """Read a Windows PE file version, returning major.minor.patch."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class FixedFileInfo(ctypes.Structure):
            _fields_ = [
                ("signature", wintypes.DWORD),
                ("struc_version", wintypes.DWORD),
                ("file_version_ms", wintypes.DWORD),
                ("file_version_ls", wintypes.DWORD),
                ("product_version_ms", wintypes.DWORD),
                ("product_version_ls", wintypes.DWORD),
                ("flags_mask", wintypes.DWORD),
                ("flags", wintypes.DWORD),
                ("os", wintypes.DWORD),
                ("type", wintypes.DWORD),
                ("subtype", wintypes.DWORD),
                ("date_ms", wintypes.DWORD),
                ("date_ls", wintypes.DWORD),
            ]

        version = ctypes.windll.version
        size = version.GetFileVersionInfoSizeW(str(path), None)
        if not size:
            return None
        block = ctypes.create_string_buffer(size)
        if not version.GetFileVersionInfoW(
            str(path), 0, size, block
        ):
            return None
        pointer = ctypes.c_void_p()
        length = wintypes.UINT()
        if not version.VerQueryValueW(
            block, "\\", ctypes.byref(pointer), ctypes.byref(length)
        ):
            return None
        info = ctypes.cast(
            pointer, ctypes.POINTER(FixedFileInfo)
        ).contents
        ms, ls = info.file_version_ms, info.file_version_ls
        parts = [ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF]
        while len(parts) > 3 and parts[-1] == 0:
            parts.pop()
        return ".".join(str(part) for part in parts[:3])
    except (AttributeError, OSError, ValueError):
        return None


def _pe_imports(path: Path) -> list[str]:
    """Read PE import DLL names using only the file image."""
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return []
    coff = pe + 4
    sections = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    if optional + optional_size > len(data):
        return []
    magic = struct.unpack_from("<H", data, optional)[0]
    data_dirs = optional + (0x60 if magic == 0x10B else 0x70)
    if magic not in (0x10B, 0x20B) or data_dirs + 16 > len(data):
        return []
    import_rva, import_size = struct.unpack_from("<II", data, data_dirs + 8)
    if not import_rva or not import_size:
        return []

    section_rows: list[tuple[int, int, int, int]] = []
    row = optional + optional_size
    for _ in range(sections):
        if row + 40 > len(data):
            break
        virtual_size, virtual_address = struct.unpack_from(
            "<II", data, row + 8
        )
        raw_size, raw_offset = struct.unpack_from("<II", data, row + 16)
        section_rows.append(
            (virtual_address, virtual_size, raw_offset, raw_size)
        )
        row += 40

    def file_offset(rva: int) -> int | None:
        for va, vsize, raw_offset, raw_size in section_rows:
            if va <= rva < va + max(vsize, raw_size):
                return raw_offset + (rva - va)
        return None

    offset = file_offset(import_rva)
    if offset is None:
        return []
    names: list[str] = []
    for _ in range(4096):
        if offset + 20 > len(data):
            break
        name_rva = struct.unpack_from("<I", data, offset + 12)[0]
        if not name_rva:
            break
        name_offset = file_offset(name_rva)
        if name_offset is None or name_offset >= len(data):
            break
        end = data.find(b"\0", name_offset, name_offset + 512)
        if end < 0:
            break
        names.append(
            data[name_offset:end].decode("ascii", errors="replace").lower()
        )
        offset += 20
    return names


def _toolchain_from_imports(paths: list[Path]) -> tuple[str | None, list[str]]:
    imported: list[str] = []
    for path in paths:
        if path.is_file():
            imported.extend(_pe_imports(path))
    if any(name.startswith(("msvcp", "vcruntime")) for name in imported):
        return "msvc", list(dict.fromkeys(
            name for name in imported
            if name.startswith(("msvcp", "vcruntime"))
        ))[:8]
    if any(name.startswith(("libstdc++", "libgcc")) for name in imported):
        return "mingw", list(dict.fromkeys(
            name for name in imported
            if name.startswith(("libstdc++", "libgcc"))
        ))[:8]
    return None, []


def _runtime_from_build_id(build_id: str | None) -> dict[str, Any]:
    if not build_id:
        return {}
    lower = build_id.lower()
    result: dict[str, Any] = {}
    if "qt5" in lower:
        result["qt_major"] = 5
    elif "qt6" in lower:
        result["qt_major"] = 6
    if "debug" in lower:
        result["debug"] = True
        result["build_type"] = "Debug"
    elif "release" in lower:
        result["debug"] = False
        result["build_type"] = "Release"
    if "msvc" in lower:
        result["toolchain"] = "msvc"
    elif "gnu" in lower or "mingw" in lower:
        result["toolchain"] = "mingw"
    return result


def _module_basename(path: str) -> str:
    """Return a lowercase basename for both Windows and POSIX paths."""
    return path.replace("\\", "/").rsplit("/", 1)[-1].lower()


def _qt_core_major(basename: str) -> int | None:
    """Return the Qt major version for a Qt Core module basename."""
    name = basename[3:] if basename.startswith("lib") else basename
    if name.startswith("qt5core"):
        return 5
    if name.startswith("qt6core"):
        return 6
    return None


def _is_runtime_library(basename: str) -> bool:
    """Accept Windows ``.dll`` and Linux ``.so``/``.so.<version>`` names."""
    return basename.endswith(".dll") or bool(_SO_VERSION_RE.search(basename))


def _shared_library_version(basename: str) -> str | None:
    match = _SO_VERSION_RE.search(basename)
    if match is None:
        return None
    return match.group(1)


def _scan_runtime_dirs(directories: list[Path]) -> dict[str, Any]:
    """Find a Qt runtime and derive toolchain only from that runtime's dir.

    A full Qt kit ``bin`` directory ships both ``Qt5Core.dll`` and
    ``Qt5Cored.dll``.  Seeing both means this directory cannot tell which one a
    process loaded, so the flavour is reported as ``None`` (unknown) and
    ``flavor_ambiguous`` is set instead of guessing from directory order: a
    wrong guess injects a DLL that links the other Qt runtime, loads a second
    Qt into the target, and then times out waiting for ``QCoreApplication``.
    """
    fallback_toolchain: str | None = None
    fallback_evidence: list[str] = []
    for directory in directories:
        if not directory.is_dir():
            continue
        try:
            entries = list(directory.iterdir())
        except OSError:
            continue
        core_entries: list[Path] = []
        core_majors: set[int] = set()
        toolchain: str | None = None
        evidence: list[str] = []
        for entry in entries:
            lower = entry.name.lower()
            major = _qt_core_major(lower)
            if major is not None and _is_runtime_library(lower):
                core_entries.append(entry)
                core_majors.add(major)
                evidence.append(str(entry))
            elif lower.startswith(("msvcp", "vcruntime")):
                toolchain = "msvc"
                evidence.append(str(entry))
            elif lower.startswith(("libstdc++", "libgcc", "libwinpthread")):
                toolchain = toolchain or "mingw"
                evidence.append(str(entry))
        if not core_entries:
            if fallback_toolchain is None and toolchain is not None:
                fallback_toolchain = toolchain
                fallback_evidence = evidence[:8]
            continue
        major = min(core_majors)
        flavors: set[bool] = set()
        versions: list[str] = []
        for entry in core_entries:
            lower = entry.name.lower()
            if _qt_core_major(lower) != major:
                continue
            if lower.endswith(".dll"):
                flavors.add(lower.endswith("d.dll"))
                version = _file_version(entry)
            else:
                flavors.add(False)
                version = _shared_library_version(lower)
            if version and version not in versions:
                versions.append(version)
        ambiguous = len(flavors) > 1
        result: dict[str, Any] = {
            "qt_major": major,
            "qt_version": versions[0] if versions else None,
            "debug": next(iter(flavors)) if len(flavors) == 1 else None,
            "toolchain": toolchain,
            "evidence": evidence[:8],
        }
        if ambiguous:
            result["flavor_ambiguous"] = True
        return result
    return {
        "qt_major": None,
        "qt_version": None,
        "debug": None,
        "toolchain": fallback_toolchain,
        "evidence": fallback_evidence,
    }


def _adjacent_runtime(executable: Path) -> dict[str, Any]:
    return _scan_runtime_dirs([executable.parent, executable.parent.parent])


def _path_runtime(process: psutil.Process) -> dict[str, Any]:
    try:
        entries = process.environ().get("PATH", "")
    except (psutil.Error, OSError):
        return {}
    directories = [
        Path(item) for item in entries.split(os.pathsep) if item.strip()
    ]
    return _scan_runtime_dirs(directories[:64])


def _module_scan_enabled() -> bool:
    value = os.environ.get(_MODULE_SCAN_ENV)
    if value is None:
        return True
    return value.strip().lower() not in {"0", "false", "no", "off"}


def _module_cache_key(process: psutil.Process) -> tuple[int, float] | None:
    info = getattr(process, "info", None)
    info = info if isinstance(info, dict) else {}
    pid = info.get("pid")
    if not isinstance(pid, int):
        try:
            pid = process.pid
        except (psutil.Error, AttributeError):
            return None
    create_time = info.get("create_time")
    if not isinstance(create_time, (int, float)):
        try:
            create_time = process.create_time()
        except (psutil.Error, OSError, AttributeError):
            create_time = 0.0
    return (pid, float(create_time))


def _scan_loaded_modules(process: psutil.Process) -> dict[str, Any]:
    try:
        entries = process.memory_maps()
    except (psutil.Error, OSError, NotImplementedError):
        return {}
    qt_major: int | None = None
    qt_version: str | None = None
    debug: bool | None = None
    toolchain: str | None = None
    evidence: list[str] = []
    try:
        for entry in entries:
            path = getattr(entry, "path", None)
            if not isinstance(path, str) or not path:
                continue
            lower = _module_basename(path)
            major = _qt_core_major(lower)
            if major is not None:
                if qt_major is None:
                    qt_major = major
                    if os.name == "nt":
                        # Windows exposes an exact PE version; the debug
                        # runtime is named Qt5Cored.dll.
                        debug = lower.endswith("d.dll")
                        qt_version = _file_version(Path(path))
                    else:
                        # Linux: libQt5Core.so.5.15.2 -> "5.15.2".
                        debug = False
                        qt_version = _shared_library_version(lower)
                evidence.append(path)
            elif lower.startswith(("msvcp", "vcruntime")):
                toolchain = "msvc"
                evidence.append(path)
            elif lower.startswith(("libstdc++", "libgcc", "libwinpthread")):
                toolchain = toolchain or "mingw"
                evidence.append(path)
    except (psutil.Error, OSError, NotImplementedError):
        return {}
    return {
        "qt_major": qt_major,
        "qt_version": qt_version,
        "debug": debug,
        "toolchain": toolchain,
        "evidence": evidence[:_MODULE_EVIDENCE_LIMIT],
    }


def _module_runtime(process: psutil.Process) -> dict[str, Any]:
    """Detect a Qt runtime from modules loaded into a live process.

    ``psutil`` implements ``memory_maps()`` with ``VirtualQueryEx`` on
    Windows and ``/proc/<pid>/maps`` on Linux; on macOS it raises
    ``NotImplementedError``. All probe failures are swallowed so process
    discovery degrades to "unknown" instead of failing.
    """
    cache_key = _module_cache_key(process)
    if cache_key is not None:
        cached = _MODULE_CACHE.get(cache_key)
        if cached is not None:
            return cached
    result = _scan_loaded_modules(process)
    if cache_key is not None:
        if len(_MODULE_CACHE) >= 4096:
            _MODULE_CACHE.clear()
        _MODULE_CACHE[cache_key] = result
    return result


_INJECT_LIBRARY_MARKERS = (
    "qt_commander_inject.dll",
    "libqt_commander_inject.so",
    "libqt_commander_inject.dylib",
)


def _is_inject_library(path: str) -> bool:
    name = _module_basename(path)
    return any(name == marker or name.startswith(marker)
               for marker in _INJECT_LIBRARY_MARKERS)


def _module_paths(process: psutil.Process) -> list[str]:
    """Return the distinct module paths mapped into a live process."""
    try:
        entries = process.memory_maps()
    except (psutil.Error, OSError, NotImplementedError):
        return []
    paths: list[str] = []
    seen: set[str] = set()
    try:
        for entry in entries:
            path = getattr(entry, "path", None)
            if not isinstance(path, str) or not path:
                continue
            key = path.lower()
            if key in seen:
                continue
            seen.add(key)
            paths.append(path)
    except (psutil.Error, OSError):
        return paths
    return paths


def _artifact_dir_of_module(path: str) -> Path:
    directory = Path(path).parent
    if directory.name.lower() in {"lib", "bin"}:
        directory = directory.parent
    return directory


def loaded_injector(
    pid: int,
    expected_artifact: str | Path | None = None,
) -> dict[str, Any]:
    """Report the ``qt_commander_inject`` library already loaded in a target.

    A library that is already mapped cannot be replaced by injecting again:
    ``LoadLibraryW`` on the same path only increments its reference count, so
    ``DllMain`` never runs and no new descriptor appears. Knowing this before
    injecting turns a silent no-op into an actionable "restart the target".
    """
    result: dict[str, Any] = {
        "loaded": False,
        "path": None,
        "paths": [],
        "artifact_dir": None,
        "artifact_dirs": [],
        "artifact": None,
        "build_id": None,
        "library_mtime": None,
        "stale": None,
        "matches_expected": None,
        "serving": None,
    }
    try:
        process = psutil.Process(pid)
        create_time = process.create_time()
    except (psutil.Error, OSError) as exc:
        result["error"] = str(exc)
        return result
    loaded = [path for path in _module_paths(process)
              if _is_inject_library(path)]
    if not loaded:
        return result
    path = loaded[0]
    directories = sorted({str(_artifact_dir_of_module(item))
                          for item in loaded})
    result.update({
        "loaded": True,
        "path": path,
        "paths": loaded,
        "artifact_dir": directories[0] if len(directories) == 1 else None,
        "artifact_dirs": directories,
    })
    manifest = artifacts.read_manifest(_artifact_dir_of_module(path))
    if manifest:
        qt = manifest.get("qt") or {}
        compiler = manifest.get("compiler") or {}
        source = manifest.get("source") or {}
        result["artifact"] = {
            "qt_version": qt.get("version"),
            "qt_debug": qt.get("debug"),
            "toolchain": compiler.get("family"),
            "compiler_version": compiler.get("version"),
            "build_type": manifest.get("build_type"),
            "version": source.get("version"),
            "commit": source.get("commit"),
        }
        result["build_id"] = artifacts.manifest_build_id(manifest)
    try:
        mtime = Path(path).stat().st_mtime
    except OSError:
        mtime = None
    if mtime is not None:
        result["library_mtime"] = mtime
        # A library rebuilt after the process started is still the old image
        # in memory: the file changed underneath it.
        result["stale"] = mtime > create_time + 1.0
    if expected_artifact:
        try:
            expected = Path(expected_artifact).resolve()
        except OSError:
            expected = None
        if expected is not None:
            result["matches_expected"] = any(
                Path(directory).resolve() == expected
                for directory in directories
            )
    return result


def injection_state(
    loaded: dict[str, Any],
    *,
    has_descriptor: bool = False,
) -> tuple[str, bool]:
    """Return ``(state, restart_required)`` for a target's injector.

    ``state`` is ``none`` (nothing loaded), ``stale`` (loaded image predates
    the artifact on disk), ``mismatch`` (loaded artifact is not the one
    ``qt_attach`` would pick) or ``loaded``.  ``restart_required`` is true
    whenever the loaded library cannot be replaced by injecting again.
    """
    if not loaded.get("loaded"):
        return "none", False
    if loaded.get("stale"):
        return "stale", True
    if loaded.get("matches_expected") is False:
        return "mismatch", True
    if not has_descriptor:
        return "loaded", True
    return "loaded", False


def list_processes(
    descriptors: dict[int, Path] | None = None,
    name: str | None = None,
    limit: int = 50,
    offset: int = 0,
    include_non_qt: bool = False,
) -> dict[str, Any]:
    """List accessible processes that look like Qt targets.

    Discovery is conservative and uses three levels, in order: a live
    descriptor, a Qt Core runtime next to the executable, and finally the Qt
    Core modules loaded into the process (via ``psutil`` memory maps). Each
    entry reports the matching level in ``evidence`` as ``"descriptor"``,
    ``"adjacent"`` or ``"module"`` (``None`` for a raw non-Qt listing). Module
    scanning is expensive, so it is cached per process and bounded by a
    wall-clock budget; ``module_scan`` reports ``enabled``/``partial``/``ms``.
    Set ``QT_COMMANDER_PROCESS_MODULES=0`` to disable it. Never injects.
    """
    descriptors = descriptors or {}
    needle = (name or "").lower()
    found: list[dict[str, Any]] = []
    runtime_cache: dict[Path, dict[str, Any]] = {}
    arch_cache: dict[Path, dict[str, Any]] = {}
    module_enabled = _module_scan_enabled()
    scan_started = time.monotonic()
    # A name filter already reduces the candidate set, so it lifts the budget.
    budget_limited = module_enabled and not needle
    deadline = scan_started + _MODULE_SCAN_BUDGET_SECONDS
    module_partial = False
    module_ms = 0.0
    for proc in psutil.process_iter(["pid", "name", "exe", "create_time"]):
        info = proc.info
        pid = info.get("pid")
        if not isinstance(pid, int):
            continue
        proc_name = info.get("name") or ""
        if needle and needle not in proc_name.lower():
            continue
        exe_value = info.get("exe")
        exe = Path(exe_value) if isinstance(exe_value, str) and exe_value else None
        descriptor = descriptors.get(pid)
        descriptor_data = _read_descriptor(descriptor) if descriptor else {}
        descriptor_qt = _descriptor_qt(descriptor_data)
        evidence: str | None = None
        # A live descriptor is definitive; avoid scanning every process's
        # neighbouring directories just to discover it. Runtime/arch results
        # are cached by directory/path because many processes share them.
        if descriptor:
            runtime = {}
            evidence = "descriptor"
        elif exe:
            runtime = runtime_cache.get(exe.parent)
            if runtime is None:
                runtime = _adjacent_runtime(exe)
                runtime_cache[exe.parent] = runtime
            if runtime.get("qt_major") is not None:
                evidence = "adjacent"
        else:
            runtime = {}
        if (
            runtime.get("qt_major") is None
            and not descriptor_qt
            and not include_non_qt
        ):
            # Third-level fallback: modules actually loaded into the process.
            # Raw ``include_non_qt`` listings do not need it: they are never
            # skipped, so scanning every process would only add latency.
            module_runtime: dict[str, Any] = {}
            if module_enabled:
                if budget_limited and time.monotonic() >= deadline:
                    module_partial = True
                else:
                    module_started = time.monotonic()
                    module_runtime = _module_runtime(proc)
                    module_ms += (time.monotonic() - module_started) * 1000.0
                if module_runtime.get("qt_major") is not None:
                    runtime = module_runtime
                    evidence = "module"
            if runtime.get("qt_major") is None:
                continue
        result: dict[str, Any] = {
            "pid": pid,
            "name": proc_name,
            "exe": str(exe) if exe else None,
        }
        if exe:
            cached_arch = arch_cache.get(exe)
            if cached_arch is None:
                cached_arch = inspect_executable(exe)
                arch_cache[exe] = cached_arch
            result.update(cached_arch)
        result.update({
            "qt_major": runtime.get("qt_major") or descriptor_qt.get("qt_major"),
            "qt_version": runtime.get("qt_version") or descriptor_qt.get("qt_version"),
            "debug": runtime.get("debug"),
            "toolchain": runtime.get("toolchain"),
            "evidence": evidence,
            "has_descriptor": descriptor is not None,
            "descriptor": str(descriptor) if descriptor else None,
            "session_id": descriptor_data.get("session_id"),
            "attach": (
                {"descriptor": str(descriptor)}
                if descriptor
                else {"pid": pid, "mode": "injection"}
            ),
        })
        if descriptor:
            build_type = _runtime_from_build_id(
                descriptor_data.get("library_build_id")
            ).get("build_type")
            if build_type:
                result["build_type"] = build_type
            try:
                detailed = target_info(pid, str(descriptor))
            except (OSError, ValueError):
                detailed = {}
            for key in ("os", "arch", "toolchain", "compiler",
                        "compiler_version", "qt_version", "debug",
                        "toolchain_evidence"):
                if detailed.get(key) is not None:
                    result[key] = detailed[key]
        found.append(result)
    total = len(found)
    page = found[offset:offset + max(1, limit)]
    payload: dict[str, Any] = {
        "count": len(page),
        "total": total,
        "limit": max(1, limit),
        "offset": offset,
        "truncated": offset + len(page) < total,
        "processes": page,
        "module_scan": {
            "enabled": module_enabled,
            "partial": module_partial,
            "ms": int(module_ms),
        },
    }
    if not page and not include_non_qt:
        payload["hint"] = (
            "no Qt target matched; retry with include_non_qt=true "
            "for a raw process list"
        )
    elif module_partial:
        payload["hint"] = (
            "module scan hit its time budget before every process was "
            "inspected; pass name= to scan a specific process"
        )
    return payload


def target_info(pid: int | None = None,
                descriptor: str | Path | None = None) -> dict[str, Any]:
    """Return architecture/Qt metadata for one process.

    The result is deliberately conservative: unknown fields are null and
    ``confidence`` describes how much was actually observed.
    """
    data: dict[str, Any] = {}
    if descriptor:
        data = _read_descriptor(descriptor)
        if pid is None and isinstance(data.get("pid"), int):
            pid = data["pid"]
    if pid is None:
        raise ValueError("pid is required")
    try:
        process = psutil.Process(pid)
        executable = Path(process.exe())
    except (psutil.Error, OSError) as exc:
        raise ValueError(f"cannot inspect pid {pid}: {exc}") from exc

    result = inspect_executable(executable)
    result["pid"] = pid
    result["pid_alive"] = psutil.pid_exists(pid)

    # Evidence order: the Qt runtime actually mapped into the process is
    # authoritative. A loader-PATH directory is not: a full Qt kit ``bin``
    # ships both the release and the debug runtime, so scanning it can only
    # produce a guess (and guessing wrong injects the other flavour).
    module_runtime: dict[str, Any] = {}
    if _module_scan_enabled():
        module_runtime = _module_runtime(process)
    detected_from: str | None = None
    if module_runtime.get("qt_major") is not None:
        runtime = dict(module_runtime)
        detected_from = "module"
    else:
        adjacent = _adjacent_runtime(executable)
        loader_runtime = _path_runtime(process)
        if adjacent.get("qt_major") is not None:
            runtime = adjacent
            detected_from = "adjacent"
        elif loader_runtime.get("qt_major") is not None:
            runtime = loader_runtime
            detected_from = "loader_path"
        elif adjacent.get("toolchain") or adjacent.get("evidence"):
            runtime = adjacent
        else:
            runtime = loader_runtime
    descriptor_runtime = _runtime_from_build_id(
        data.get("library_build_id") if data else None
    )
    descriptor_qt = _descriptor_qt(data)
    if detected_from is None and data:
        detected_from = "descriptor"
    import_paths = [
        Path(path) for path in runtime.get("evidence", []) if path
    ]
    import_toolchain, import_evidence = _toolchain_from_imports(
        import_paths + [executable]
    )
    if import_toolchain:
        runtime["toolchain"] = import_toolchain
        runtime["toolchain_evidence"] = [
            f"import:{name}" for name in import_evidence
        ]
    for key in ("qt_major", "qt_version", "debug", "build_type", "toolchain"):
        if runtime.get(key) is None:
            value = descriptor_qt.get(key, descriptor_runtime.get(key))
            if value is not None:
                runtime[key] = value
    if runtime.get("qt_major") is None and runtime.get("qt_version"):
        try:
            runtime["qt_major"] = int(str(runtime["qt_version"]).split(".")[0])
        except (ValueError, IndexError):
            pass
    if descriptor_runtime and not runtime.get("evidence"):
        runtime["evidence"] = [f"descriptor:{data.get('library_build_id')}"]
    result.update(runtime)
    result["detected_from"] = detected_from
    system = platform.system().lower()
    result["os"] = (
        "windows" if system == "windows"
        else "linux" if system == "linux"
        else "macos" if system == "darwin"
        else system or "unknown"
    )
    result["compiler"] = result.get("toolchain")
    result["compiler_version"] = None
    if isinstance(data.get("compiler"), dict):
        family = data["compiler"].get("family")
        version = data["compiler"].get("version")
        if isinstance(family, str):
            result["compiler"] = family
        if isinstance(version, str):
            result["compiler_version"] = version
    if isinstance(data.get("os"), str):
        result["os"] = data["os"]
    if isinstance(data.get("arch"), str):
        result["arch"] = data["arch"]
    result["qt_debug"] = result.get("debug")

    # Which artifact qt_attach would inject, plus what is already loaded: this
    # is what makes a failed inject self-explanatory.
    expected = artifacts.find(result)
    expected_dir: str | None = None
    if expected.get("ok"):
        expected_dir = str(expected["directory"])
        manifest = expected["manifest"]
        source = manifest.get("source") or {}
        result["expected_artifact"] = {
            "directory": expected_dir,
            "build_id": artifacts.manifest_build_id(manifest),
            "build_type": manifest.get("build_type"),
            "qt_debug": (manifest.get("qt") or {}).get("debug"),
            "version": source.get("version"),
            "commit": source.get("commit"),
        }
    else:
        result["expected_artifact"] = None
        result["expected_artifact_error"] = {
            key: expected.get(key)
            for key in ("error", "message", "hint", "candidates")
            if expected.get(key) is not None
        }
    loaded = loaded_injector(pid, expected_artifact=expected_dir)
    loaded["serving"] = bool(data)
    state, restart_required = injection_state(
        loaded, has_descriptor=bool(data)
    )
    result["loaded_injector"] = loaded
    result["injection_state"] = state
    result["restart_required"] = restart_required

    if data:
        result["descriptor"] = {
            "session_id": data.get("session_id"),
            "library_build_id": data.get("library_build_id"),
            "library_version": data.get("library_version"),
            "started_at": data.get("started_at"),
        }
        if isinstance(data.get("qt"), dict):
            result["descriptor"]["qt"] = data["qt"]
        # A descriptor without a Qt DLL still proves the runtime was loaded.
        if result.get("confidence") == "low" and data.get("library_build_id"):
            result["confidence"] = "medium"
    return result
