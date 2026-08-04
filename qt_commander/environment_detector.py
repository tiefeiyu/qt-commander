"""Auto-detect build environments on Windows: Visual Studio and Qt installations.

Standard detection methods:
  - VS:  vswhere.exe (VS 2017+), registry fallback (legacy VS)
  - Qt:  registry (Trolltech keys), QTDIR env var, qmake on PATH,
         structured C:\\Qt\\ layout (known install location pattern),
         fixed-drive scan for the standard <root>/<ver>/<compiler> layout
         (covers custom install roots such as C:\\Software\\Qt)

The drive scan probes only the well-known Qt kit layout
(<root>/<version>/<compiler>/bin/qmake.exe), prunes noise directories, and
validates every candidate with ``qmake -query``, so it never guesses.
"""

import json
import os
import re
import subprocess
import sys
import winreg
from pathlib import Path


# ═══════════════════════════════════════════════════════════════════════════
# Visual Studio detection
# ═══════════════════════════════════════════════════════════════════════════

def _vswhere_path() -> Path | None:
    """Return path to vswhere.exe if it exists."""
    pf86 = os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)")
    p = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return p if p.exists() else None


def _find_vcvars_scripts(inst_path: Path) -> list[dict]:
    """Enumerate available vcvars*.bat scripts in the Auxiliary Build dir."""
    scripts: list[dict] = []
    build_aux = inst_path / "VC" / "Auxiliary" / "Build"
    if not build_aux.exists():
        return scripts

    # Ordered so the most common native-hosted scripts appear first
    known: dict[str, tuple[str, str]] = {
        "vcvars64.bat":          ("x64",   "x64 native tools"),
        "vcvars32.bat":          ("x86",   "x86 native tools"),
        "vcvars86_amd64.bat":    ("x64",   "x86 cross to x64"),
        "vcvarsx86_amd64.bat":   ("x64",   "x86 cross to x64"),
        "vcvarsamd64_x86.bat":   ("x86",   "x64 cross to x86"),
        "vcvars64_x86.bat":      ("x86",   "x64 cross to x86"),
        "vcvarsx86_arm64.bat":   ("arm64", "x86 cross to ARM64"),
        "vcvarsamd64_arm64.bat": ("arm64", "x64 cross to ARM64"),
        "vcvarsall.bat":         ("multi", "all architectures (needs arg)"),
    }

    for name, (arch, desc) in known.items():
        full = build_aux / name
        if full.exists():
            scripts.append({"name": name, "path": str(full),
                            "arch": arch, "description": desc})
    return scripts


def detect_vs_environments() -> list[dict]:
    """Return a list of detected Visual Studio installations.

    Each entry:
      display_name  – human-readable name (e.g. "Visual Studio 2022 Community")
      version        – raw version string from vswhere / registry
      install_path   – root installation directory
      vcvars         – list of {name, path, arch, description}
    """
    if sys.platform != "win32":
        return []

    results: list[dict] = []
    seen: set[str] = set()

    def _add(entry: dict | None) -> None:
        if entry is None:
            return
        key = entry["install_path"].lower()
        if key not in seen:
            seen.add(key)
            results.append(entry)

    # ── Method 1: registry — modern VS 2019+ via Capabilities keys ────
    #    This works without subprocess and is the most reliable method.
    for entry in _detect_vs_modern_registry():
        _add(entry)

    # ── Method 2: vswhere (VS 2017+) — richer metadata when available ─
    vswhere = _vswhere_path()
    if vswhere is not None:
        try:
            proc = subprocess.run(
                [str(vswhere), "-all", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-format", "json", "-nocolor"],
                capture_output=True, text=True, encoding="utf-8",
                timeout=30,
            )
            if proc.returncode == 0 and proc.stdout and proc.stdout.strip():
                instances = json.loads(proc.stdout)
                for inst in instances:
                    ip = Path(inst.get("installationPath", ""))
                    if not ip.exists():
                        continue
                    vcvars = _find_vcvars_scripts(ip)
                    if not vcvars:
                        continue
                    entry = {
                        "display_name": inst.get("displayName", ip.name),
                        "version": inst.get("installationVersion", ""),
                        "install_path": str(ip),
                        "vcvars": vcvars,
                    }
                    # vswhere gives richer metadata; prefer it over registry
                    key = str(ip).lower()
                    if key in seen:
                        # Replace registry entry with richer vswhere entry
                        for i, r in enumerate(results):
                            if r["install_path"].lower() == key:
                                results[i] = entry
                                break
                    else:
                        seen.add(key)
                        results.append(entry)
        except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError, UnicodeError):
            pass

    # ── Method 3: registry fallback (legacy VS 2013–2017) ─────────────
    for entry in _detect_vs_registry():
        _add(entry)

    return results


def _detect_vs_modern_registry() -> list[dict]:
    """Detect VS 2019+ via registry Capabilities keys.

    Enumerates subkeys of HKLM\\SOFTWARE\\WOW6432Node\\Microsoft matching
    ``VisualStudio_*``, reads the ``Capabilities\\ApplicationDescription``
    value, and resolves the install root from the path embedded in that value.
    """
    results: list[dict] = []
    vs_pattern = re.compile(r"^VisualStudio_")

    for hive, base in [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft"),
    ]:
        try:
            key = winreg.OpenKey(hive, base, 0,
                                 winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            i = 0
            while True:
                try:
                    subkey_name = winreg.EnumKey(key, i)
                    i += 1
                except OSError:
                    break

                m = vs_pattern.match(subkey_name)
                if not m:
                    continue

                # Read Capabilities\ApplicationDescription
                caps_path = rf"{base}\{subkey_name}\Capabilities"
                try:
                    caps_key = winreg.OpenKey(hive, caps_path, 0,
                                              winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
                    try:
                        desc, _ = winreg.QueryValueEx(caps_key, "ApplicationDescription")
                    except (FileNotFoundError, OSError):
                        winreg.CloseKey(caps_key)
                        continue
                    winreg.CloseKey(caps_key)
                except (FileNotFoundError, OSError):
                    continue

                # ApplicationDescription is like:
                #   "@C:\\...\\2022\\Community\\Common7\\IDE\\devenvdesc.dll,-1004"
                # The install root is 3 levels up from Common7\\IDE\\devenvdesc.dll
                desc_clean = desc.lstrip("@").split(",")[0]
                desc_path = Path(desc_clean)
                # Navigate up: .../Common7/IDE/devenvdesc.dll → .../<edition>
                inst_path = desc_path.parent.parent.parent

                if not inst_path.exists():
                    continue

                vcvars = _find_vcvars_scripts(inst_path)
                if not vcvars:
                    continue

                # Derive a display name from the path (e.g. "2022\Community")
                vs_year = inst_path.parent.name if inst_path.parent != inst_path else ""
                vs_edition = inst_path.name
                display_name = f"Visual Studio {vs_year} {vs_edition}"
                vs_ver = vs_year

                results.append({
                    "display_name": display_name,
                    "version": vs_ver,
                    "install_path": str(inst_path),
                    "vcvars": vcvars,
                })
            winreg.CloseKey(key)
        except (FileNotFoundError, OSError):
            continue

    return results


def _detect_vs_registry() -> list[dict]:
    """Registry-based fallback for older Visual Studio versions."""
    results: list[dict] = []

    # SxS key (VS 2017 style)
    for hive, base in [(winreg.HKEY_LOCAL_MACHINE,
                         r"SOFTWARE\Microsoft\VisualStudio\SxS\VS7"),
                        (winreg.HKEY_LOCAL_MACHINE,
                         r"SOFTWARE\WOW6432Node\Microsoft\VisualStudio\SxS\VS7")]:
        try:
            key = winreg.OpenKey(hive, base, 0,
                                 winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            try:
                for vs_ver in ("15.0",):  # VS 2017
                    try:
                        ip, _ = winreg.QueryValueEx(key, vs_ver)
                        inst_path = Path(ip).resolve()
                        vcvars = _find_vcvars_scripts(inst_path)
                        if vcvars:
                            results.append({
                                "display_name": f"Visual Studio 2017",
                                "version": vs_ver,
                                "install_path": str(inst_path),
                                "vcvars": vcvars,
                            })
                    except (FileNotFoundError, OSError):
                        continue
            finally:
                winreg.CloseKey(key)
        except (FileNotFoundError, OSError):
            continue

    # Legacy Setup\VC keys (VS 2013–2015)
    for vs_ver, vs_label in [("14.0", "Visual Studio 2015"),
                              ("12.0", "Visual Studio 2013")]:
        for hive in [winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER]:
            try:
                kp = rf"SOFTWARE\Microsoft\VisualStudio\{vs_ver}\Setup\VC"
                key = winreg.OpenKey(hive, kp, 0,
                                     winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
                try:
                    product_dir, _ = winreg.QueryValueEx(key, "ProductDir")
                    # ProductDir ends with VC\ (or has a trailing VC dir);
                    # the install root is two levels up.
                    inst_path = Path(product_dir).resolve()
                    if inst_path.name.lower() == "vc":
                        inst_path = inst_path.parent
                    if inst_path.name.lower() == "tools":
                        inst_path = inst_path.parent.parent

                    vcvars = _find_vcvars_scripts(inst_path)
                    # Also check legacy location: <inst>\VC\bin\
                    for bat in sorted(
                        inst_path.glob("VC/bin/*/vcvars*.bat")
                    ):
                        name = bat.name
                        if not any(v["name"] == name for v in vcvars):
                            arch = "x64" if "64" in name else "x86"
                            vcvars.append({
                                "name": name, "path": str(bat), "arch": arch,
                                "description": "",
                            })
                    if vcvars:
                        results.append({
                            "display_name": vs_label,
                            "version": vs_ver,
                            "install_path": str(inst_path),
                            "vcvars": vcvars,
                        })
                finally:
                    winreg.CloseKey(key)
            except (FileNotFoundError, OSError):
                continue

    return results


# ═══════════════════════════════════════════════════════════════════════════
# Qt detection
# ═══════════════════════════════════════════════════════════════════════════

def _run_qmake_query(qmake: Path) -> dict[str, str] | None:
    """Run ``qmake -query``, return parsed key→value dict, or None."""
    try:
        proc = subprocess.run(
            [str(qmake), "-query"],
            capture_output=True, text=True, timeout=10, check=False,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None

    if proc.returncode != 0 or not proc.stdout.strip():
        return None

    props: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if ":" in line:
            key, _, value = line.partition(":")
            props[key.strip()] = value.strip()
    return props


def _inspect_qmake_env(qmake: Path) -> dict | None:
    """Given a qmake.exe path, extract Qt metadata via ``qmake -query``
    and find the corresponding environment setup script.
    """
    props = _run_qmake_query(qmake)
    if not props:
        return None

    prefix = props.get("QT_INSTALL_PREFIX", "")
    if not prefix:
        return None

    bin_dir = qmake.parent

    # Qt5 → qtenv2.bat, Qt6 → qt-cmake.bat or qtenv2.bat
    qtenv: str = ""
    for candidate in ("qtenv2.bat", "qt-cmake.bat"):
        p = bin_dir / candidate
        if p.exists():
            qtenv = str(p)
            break

    version = props.get("QT_VERSION", "")
    compiler = bin_dir.parent.name if bin_dir.parent != bin_dir else ""
    is_64 = "64" in compiler.lower()

    return {
        "version": version,
        "compiler": compiler,
        "install_prefix": prefix,
        "qmake_path": str(qmake),
        "qtenv_path": qtenv,
        "arch": "x64" if is_64 else "x86",
    }


def _find_qmake_on_path() -> list[Path]:
    """Locate qmake.exe executables by walking PATH entries."""
    found: list[Path] = []
    seen: set[str] = set()

    for entry in os.environ.get("PATH", "").split(os.pathsep):
        entry = entry.strip()
        if not entry:
            continue
        normalized = os.path.normpath(entry).lower()
        if normalized in seen:
            continue
        seen.add(normalized)

        candidate = Path(entry) / "qmake.exe"
        if candidate.exists():
            found.append(candidate)

    return found


def _detect_qt_registry() -> list[dict]:
    """Detect Qt installations via Windows registry (Trolltech keys)."""
    results: list[dict] = []

    reg_bases = [
        (winreg.HKEY_CURRENT_USER,
         r"Software\Trolltech\Versions"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"Software\Trolltech\Versions"),
    ]

    for hive, base in reg_bases:
        try:
            key = winreg.OpenKey(hive, base, 0,
                                 winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            try:
                # Iterate subkeys (each is a Qt version string)
                i = 0
                while True:
                    try:
                        subkey_name = winreg.EnumKey(key, i)
                        i += 1

                        sk = winreg.OpenKey(hive, rf"{base}\{subkey_name}",
                                            0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
                        try:
                            install_dir_val, _ = winreg.QueryValueEx(sk, "InstallDir")
                        except (FileNotFoundError, OSError):
                            winreg.CloseKey(sk)
                            continue
                        winreg.CloseKey(sk)

                        install_dir = Path(install_dir_val)
                        # Normalize — InstallDir may end with /bin
                        if install_dir.name.lower() == "bin":
                            install_dir = install_dir.parent

                        qmake = install_dir / "bin" / "qmake.exe"
                        if qmake.exists():
                            info = _inspect_qmake_env(qmake)
                            if info:
                                results.append(info)
                    except OSError:
                        break
            finally:
                winreg.CloseKey(key)
        except (FileNotFoundError, OSError):
            continue

    return results


def _detect_qt_structured() -> list[dict]:
    """Look for Qt installations under the standard C:\\Qt\\ layout.

    This is a pattern-based lookup in a well-known directory, not blind
    scanning.  The Qt installer always uses ``C:\\Qt\\<version>\\<compiler>``.
    """
    results: list[dict] = []

    qt_root = Path("C:/Qt")
    if not qt_root.exists():
        return results

    # C:\Qt\*\*\bin\qmake.exe  (e.g. C:\Qt\5.15.2\msvc2019_64\bin\qmake.exe)
    for qmake in sorted(qt_root.glob("*/*/bin/qmake.exe")):
        # Only pick the first match per compiler dir
        info = _inspect_qmake_env(qmake)
        if info:
            results.append(info)

    return results


# Directories never worth probing for Qt kits during the drive scan.
_SKIP_DIR_NAMES = {
    "$recycle.bin", "appdata", "cygwin64", "documents and settings",
    "msys64", "mingw64", "node_modules", "perflogs", "program files",
    "program files (x86)", "programdata", "system volume information",
    "temp", "tmp", "users", "venv", ".venv", "windows",
}


def _scan_drive_for_qmake(drive_root: Path, max_depth: int = 4) -> list[Path]:
    """Find qmake.exe files in the Qt kit layout under ``drive_root``.

    Probes directories up to ``max_depth`` levels deep for a ``bin/qmake.exe``
    child, skipping known noise directories.  The Qt installer layout is
    ``<root>/<version>/<compiler>/bin/qmake.exe``, so any root location is
    found as long as it sits within the depth limit.
    """
    found: list[Path] = []
    stack: list[tuple[Path, int]] = [(drive_root, 0)]
    while stack:
        base, depth = stack.pop()
        if depth > max_depth:
            continue
        try:
            entries = list(base.iterdir())
        except OSError:
            continue
        for entry in entries:
            try:
                if not entry.is_dir():
                    continue
            except OSError:
                continue
            if entry.name.lower() in _SKIP_DIR_NAMES:
                continue
            qmake = entry / "bin" / "qmake.exe"
            if qmake.is_file():
                found.append(qmake)
            else:
                stack.append((entry, depth + 1))
    return found


def _detect_qt_drive_scan() -> list[dict]:
    """Scan fixed drives for Qt kits in the standard installer layout.

    Catches custom install roots (e.g. ``C:\\Software\\Qt``) that the
    well-known-location methods miss.  Every candidate is validated by
    running ``qmake -query`` before being reported.
    """
    results: list[dict] = []

    import ctypes

    kernel32 = ctypes.windll.kernel32
    bitmask = kernel32.GetLogicalDrives()
    seen_qmake: set[str] = set()

    for i in range(26):
        if not (bitmask & (1 << i)):
            continue
        drive = f"{chr(ord('A') + i)}:\\"
        if kernel32.GetDriveTypeW(drive) != 3:
            continue  # not DRIVE_FIXED

        for qmake in _scan_drive_for_qmake(Path(drive)):
            key = str(qmake).lower()
            if key in seen_qmake:
                continue
            seen_qmake.add(key)
            info = _inspect_qmake_env(qmake)
            if info:
                results.append(info)

    return results


def detect_qt_environments() -> list[dict]:
    """Return a list of detected Qt installations.

    Detection order (first hit per install wins, deduplicated by prefix):
      1. ``QTDIR`` environment variable
      2. ``qmake.exe`` on ``PATH``
      3. Windows registry (Trolltech keys)
      4. Structured ``C:\\Qt`` layout (known Qt installer pattern)
      5. Fixed-drive scan for the standard kit layout (custom roots)

    Each entry:
      version        – Qt version string (e.g. "5.15.2")
      compiler       – compiler tag (e.g. "msvc2019_64", "mingw81_64")
      install_prefix – QT_INSTALL_PREFIX from qmake -query
      qmake_path     – full path to qmake.exe
      qtenv_path     – path to qtenv2.bat / qt-cmake.bat (empty if unfound)
      arch           – "x64" or "x86"
    """
    if sys.platform != "win32":
        return []

    results: list[dict] = []
    seen: set[str] = set()

    def _add(info: dict | None) -> None:
        if info is None:
            return
        prefix = info.get("install_prefix", "")
        if prefix and prefix not in seen:
            seen.add(prefix)
            compiler = info.get("compiler", "").lower()
            kit = ""
            if "mingw" in compiler:
                kit = "mingw"
            elif "msvc" in compiler:
                kit = "msvc"
            sorted_info = {
                "version": info.get("version", ""),
                "compiler": info.get("compiler", ""),
                "kit": kit,  # "msvc" | "mingw" | "" — which qt_build toolchain to use
                "install_prefix": info.get("install_prefix", ""),
                "qmake_path": info.get("qmake_path", ""),
                "qtenv_path": info.get("qtenv_path", ""),
                "arch": info.get("arch", ""),
            }
            results.append(sorted_info)

    # 1. QTDIR env var
    qtdir = os.environ.get("QTDIR", "")
    if qtdir:
        qmake = Path(qtdir) / "bin" / "qmake.exe"
        if qmake.exists():
            _add(_inspect_qmake_env(qmake))

    # 2. qmake on PATH
    for qmake in _find_qmake_on_path():
        _add(_inspect_qmake_env(qmake))

    # 3. Registry
    for info in _detect_qt_registry():
        _add(info)

    # 4. Structured C:\Qt layout
    for info in _detect_qt_structured():
        _add(info)

    # 5. Fixed-drive scan (custom install roots like C:\Software\Qt)
    for info in _detect_qt_drive_scan():
        _add(info)

    return results


# ═══════════════════════════════════════════════════════════════════════════
# MinGW toolchain detection
# ═══════════════════════════════════════════════════════════════════════════

_MINGW_RE = re.compile(r"^(?:llvm-)?mingw", re.IGNORECASE)


def _gcc_version(gpp: Path) -> str:
    """Extract the g++ version string (e.g. '13.1.0'), or '' on failure."""
    try:
        proc = subprocess.run(
            [str(gpp), "--version"], capture_output=True, text=True,
            timeout=10, check=False,
        )
        line = proc.stdout.splitlines()[0] if proc.stdout else ""
        m = re.search(r"\d+\.\d+\.\d+", line)
        return m.group(0) if m else ""
    except (subprocess.TimeoutExpired, OSError):
        return ""


def detect_mingw_toolchains() -> list[dict]:
    """Locate MinGW toolchains bundled with Qt installers.

    The Qt online installer places them under ``<Qt root>/Tools/`` with
    names like ``mingw810_64`` / ``mingw1310_64`` / ``llvm-mingw1706_64``.
    Each candidate is validated by the presence of ``g++.exe`` in its
    ``bin`` dir, and the gcc version is probed via ``g++ --version``.

    Each entry:
      name        – toolchain directory name (e.g. "mingw1310_64")
      path        – bin dir (the value to pass as vcvars_path for
                    toolchain="mingw" in qt_build)
      root        – toolchain root directory
      gcc_version – gcc version string (e.g. "13.1.0"), '' if unprobed
    """
    results: list[dict] = []
    seen: set[str] = set()
    for qt_root in (Path("C:/Qt"), Path("C:/Software/Qt")):
        tools = qt_root / "Tools"
        if not tools.is_dir():
            continue
        for entry in sorted(tools.iterdir()):
            if not _MINGW_RE.match(entry.name):
                continue
            bin_dir = entry / "bin"
            gpp = bin_dir / "g++.exe"
            if not gpp.exists():
                continue
            key = str(bin_dir).lower()
            if key in seen:
                continue
            seen.add(key)
            results.append({
                "name": entry.name,
                "path": str(bin_dir),
                "root": str(entry),
                "gcc_version": _gcc_version(gpp),
            })
    return results
