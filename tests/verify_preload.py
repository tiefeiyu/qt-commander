"""End-to-end verification: the injector preloads the dependency closure of
libqt-commander.dll into the target process, so clean windeployqt-only app
directories attach without any manual Qt DLL copies.

Windows-only test (requires windeployqt + PE DLLs).  On Linux, exits 0.
"""
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

if sys.platform != "win32":
    # Linux: minimal verification — check that the built .so loads with Qt.
    _qtc_bin = REPO / ".qt-commander" / "bin"
    _so_path = _qtc_bin / "libqt-commander.so"
    if not _so_path.exists():
        _alt = REPO / "build" / "linux5" / "src" / "library" / "libqt-commander.so"
        if _alt.exists():
            _so_path = _alt
        else:
            print(f"verify_preload: SKIP — {_so_path} not found (run qt_build first)")
            sys.exit(0)
    import ctypes
    try:
        # os.RTLD_NOW=2, os.RTLD_GLOBAL=256 (POSIX dlopen flags)
        _handle = ctypes.CDLL(str(_so_path), mode=os.RTLD_NOW | os.RTLD_GLOBAL)
        print(f"verify_preload: OK — {_so_path} loaded successfully")
    except OSError as e:
        print(f"verify_preload: FAIL — {e}")
        sys.exit(1)
    sys.exit(0)
sys.path.insert(0, str(REPO))

from qt_commander.errors import InjectionError  # noqa: E402
from qt_commander.rpc_client import inject_and_connect  # noqa: E402
from qt_commander.session import Session  # noqa: E402

# QTC_DIR overrides the deployment root (useful for regression runs against
# an alternate Qt-major deployment without clobbering .qt-commander/bin).
QTC_ROOT = Path(os.environ.get("QTC_DIR", REPO / ".qt-commander"))
QTC_BIN = QTC_ROOT / "bin"
INJECTOR = QTC_BIN / "qt-injector.exe"
LIBRARY = QTC_BIN / "libqt-commander.dll"
QT_MAJOR = ""  # filled in by detect_deployment() inside main()
QT_KIT = ""    # "msvc" | "mingw"
QT_DLL_SUFFIX = ""  # "d" for debug deployments

# Qt bin dir: QT_BIN env var wins (run_all_tests.py sets it), otherwise the
# Qt<major>_DIR from the test build's CMakeCache, otherwise the repo default.
_qt_bin = Path(os.environ.get("QT_BIN", "")) if os.environ.get("QT_BIN") \
    else None
if not _qt_bin or not _qt_bin.is_dir():
    _cache = QTC_ROOT / "test-apps-build" / "CMakeCache.txt"
    _qt_bin = None
    if _cache.exists():
        for _key in ("Qt5_DIR", "Qt6_DIR"):
            for _line in _cache.read_text(encoding="utf-8",
                                          errors="ignore").splitlines():
                if _line.startswith(f"{_key}="):
                    _cand = Path(_line.split("=", 1)[1]).parents[2] / "bin"
                    if _cand.is_dir():
                        _qt_bin = _cand
                        break
            if _qt_bin:
                break
    if not _qt_bin:
        _default = Path(r"C:\Software\Qt\5.15.2\msvc2019_64\bin")
        _qt_bin = _default if _default.is_dir() else None
QT_BIN = _qt_bin
WINDEPLOYQT = QT_BIN / "windeployqt.exe" if QT_BIN else None
QML_SRC = REPO / "tests" / "test-apps" / "qml"
WIDGET_SRC = REPO / "tests" / "test-apps" / "widget"

# Test apps: ctest passes its own build tree's apps (which match the
# deployed build type -- Debug tree + Debug deployment).  Standalone runs
# fall back to the qt_build-deployed apps under QTC_ROOT.
QML_APP = Path(os.environ["QT_QML_APP"]) if os.environ.get("QT_QML_APP") \
    else QTC_ROOT / "qml-app" / "bin" / "qt-qml-test.exe"
WIDGET_APP = Path(os.environ["QT_WIDGET_APP"]) if os.environ.get("QT_WIDGET_APP") \
    else QTC_ROOT / "test-app" / "bin" / "qt-widget-test.exe"

failures = []


def check(cond: bool, msg: str):
    tag = "PASS" if cond else "FAIL"
    print(f"  [{tag}] {msg}")
    if not cond:
        failures.append(msg)


# ---------------------------------------------------------------------------
# Deploy helpers
# ---------------------------------------------------------------------------

_QT5_CLOSURE = ("Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll", "Qt5Quick.dll",
                "Qt5Qml.dll", "Qt5QmlModels.dll", "Qt5Network.dll")
_COMPILER_RUNTIME = ("libgcc_s_seh-1.dll", "libstdc++-6.dll",
                     "libwinpthread-1.dll")


def deploy_closure_qt5_minqw(app_exe: Path, qmldir: Path | None,
                             dest: Path) -> None:
    """Qt 5's MinGW windeployqt cannot start at all (its QGuiApplication
    never finds the platform plugin), so deploy the app's closure manually:
    the Qt DLL set the app actually links (qmldir apps take the QML set,
    others just Core/Gui/Widgets), the compiler runtime (from the
    deployment dir), the platform plugin, and the QML modules."""
    dlls = _QT5_CLOSURE if qmldir else _QT5_CLOSURE[:3]
    for dll in dlls:
        src = QT_BIN / dll
        if src.exists():
            shutil.copy2(src, dest / dll)
    for rt in _COMPILER_RUNTIME:
        src = QTC_BIN / rt
        if src.exists():
            shutil.copy2(src, dest / rt)
    qt_root = QT_BIN.parent
    shutil.copytree(qt_root / "plugins" / "platforms", dest / "platforms")
    if qmldir:
        qml_root = qt_root / "qml"
        shutil.copytree(qml_root / "QtQuick.2", dest / "qml" / "QtQuick.2")
        shutil.copytree(qml_root / "QtQuick" / "Window.2",
                        dest / "qml" / "QtQuick" / "Window.2")
        shutil.copytree(qml_root / "QtQml", dest / "qml" / "QtQml")


def deploy_windeployqt(app_exe: Path, qmldir: Path | None, dest: Path) -> None:
    """Fresh windeployqt-only deployment into dest (clean dir)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    shutil.copy2(app_exe, dest / app_exe.name)
    if QT_KIT == "mingw" and QT_MAJOR == "5":
        deploy_closure_qt5_minqw(app_exe, qmldir, dest)
        return
    deploy_flavor = "--debug" if QT_DLL_SUFFIX else "--release"
    args = [str(WINDEPLOYQT), deploy_flavor, "--no-translations"]
    if QT_KIT != "mingw" and not QT_DLL_SUFFIX:
        # Release MSVC apps find their CRT in the system; MinGW apps and
        # Debug MSVC apps do not (Debug CRT is never redistributed, and
        # the Qt kit's runtime sits next to the kit, not the app), so
        # windeployqt must deploy the compiler runtime for those.
        args += ["--no-compiler-runtime"]
    if qmldir:
        args += ["--qmldir", str(qmldir)]
    args.append(str(dest / app_exe.name))
    subprocess.run(args, check=True, capture_output=True)
    if QT_KIT == "mingw":
        # The deployed app must run on the same compiler runtime as the
        # injected library; windeployqt ships the Qt kit's (possibly older)
        # runtime.  Take the runtime from the deployment dir.
        for rt in _COMPILER_RUNTIME:
            src = QTC_BIN / rt
            if src.exists():
                shutil.copy2(src, dest / rt)
    # The widget app loads its UI from code; the QML app embeds the scene in
    # a qrc, so no extra source files are needed.


def missing_dlls(app_dir: Path, names: list[str]) -> list[str]:
    return [n for n in names if not (app_dir / n).exists()]


def qdll(short: str) -> str:
    """Qt<major><short>.dll, e.g. qdll("Widgets") -> Qt6Widgets.dll (or
    Qt6Widgetsd.dll for a debug deployment)."""
    return f"Qt{QT_MAJOR}{short}{QT_DLL_SUFFIX}.dll"


_MINGW_RUNTIME_DLLS = ("libgcc_s_seh-1.dll", "libstdc++-6.dll",
                       "libwinpthread-1.dll")


def detect_deployment() -> tuple[str, str, str]:
    """Derive (Qt major, kit, dll suffix) from libqt-commander.dll's import
    closure.

    kit is "msvc" or "mingw", identified by the CRT runtime DLLs the
    library imports (MinGW Qt DLLs import libgcc_s_seh-1.dll etc.).
    The dll suffix is "" for release and "d" for debug deployments.
    """
    proc = subprocess.run(
        [str(INJECTOR), "--list-deps", str(LIBRARY),
         "--search-dir", str(QTC_BIN)],
        capture_output=True, text=True, timeout=30)
    deps = json.loads(proc.stdout)["deps"]
    names = {Path(p).name.lower() for p in deps}
    for n in sorted(names):
        if n.startswith("qt6core"):
            major, suffix = "6", n[len("qt6core"):-len(".dll")]
            break
        if n.startswith("qt5core"):
            major, suffix = "5", n[len("qt5core"):-len(".dll")]
            break
    else:
        raise RuntimeError(f"cannot determine Qt major from library closure: "
                           f"{sorted(names)}")
    kit = "mingw" if any(n in names for n in _MINGW_RUNTIME_DLLS) else "msvc"
    return major, kit, suffix


def qt_bin_major(bin_dir: Path) -> str | None:
    """Probe which Qt major a bin dir belongs to (by its QtCore.dll)."""
    if (bin_dir / "Qt6Core.dll").exists():
        return "6"
    if (bin_dir / "Qt5Core.dll").exists():
        return "5"
    return None


def qt_bin_kit(bin_dir: Path) -> str | None:
    """Probe whether a Qt bin dir is MinGW or MSVC (by its runtime DLLs)."""
    if qt_bin_major(bin_dir) is None:
        return None
    if any((bin_dir / n).exists() for n in _MINGW_RUNTIME_DLLS):
        return "mingw"
    return "msvc"


# Canonical installs for each (major, kit), used when the QT_BIN from the
# build tree (ctest passes its own tree's Qt bin) does not match the
# deployed library.  A wrong-major windeployqt rejects the exe outright,
# and a wrong-kit one deploys DLLs that crash the app (entry point not
# found), so windeployqt must follow the deployment exactly.
_CANONICAL_QT_BIN = {
    ("5", "msvc"):  Path(r"C:\Software\Qt\5.15.2\msvc2019_64\bin"),
    ("5", "mingw"): Path(r"C:\Software\Qt\5.15.2\mingw81_64\bin"),
    ("6", "msvc"):  Path(r"C:\Software\Qt\6.8.3\msvc2022_64\bin"),
    ("6", "mingw"): Path(r"C:\Software\Qt\6.8.3\mingw_64\bin"),
}


def start_app(app_exe: Path) -> subprocess.Popen:
    proc = subprocess.Popen([str(app_exe)], cwd=str(app_exe.parent))
    time.sleep(3)
    assert proc.poll() is None, f"app exited early with {proc.returncode}"
    return proc


def stop_app(proc: subprocess.Popen):
    if proc.poll() is None:
        proc.kill()  # TerminateProcess: Qt apps may ignore WM_CLOSE
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


def eject(pid: int):
    subprocess.run([str(INJECTOR), "--eject", str(pid), str(LIBRARY)],
                   capture_output=True, timeout=30)


async def attach_and_probe(session: Session, pid: int, port_file: Path,
                           btn_name: str, status_name: str):
    await inject_and_connect(pid, LIBRARY, port_file, INJECTOR, session)

    # findElement -> clickRegion -> property read-back.
    r = await session.send_rpc("qt.findElement",
                               {"query": {"object_name": btn_name}})
    assert r.get("ok") and r.get("elements"), f"findElement failed: {r}"
    btn_id = r["elements"][0]["id"]

    r = await session.send_rpc("qt.findElement",
                               {"query": {"object_name": status_name}})
    assert r.get("ok") and r.get("elements"), f"findElement {status_name}: {r}"
    status_id = r["elements"][0]["id"]

    r = await session.send_rpc("qt.clickRegion",
                               {"element_id": btn_id, "button": "left",
                                "modifiers": []})
    assert r.get("ok"), f"clickRegion failed: {r}"

    r = await session.send_rpc("qt.getProperty",
                               {"element_id": status_id, "name": "text"})
    assert r.get("ok"), f"getProperty failed: {r}"
    return r.get("value", "")


async def scenario_qml(work: Path) -> None:
    print(f"\n[Scenario A] QML app, {qdll('Widgets')} removed from app dir")
    app_dir = work / "app-qml"
    deploy_windeployqt(QML_APP, QML_SRC, app_dir)
    # windeployqt happens to deploy Qt<major>Widgets for QML apps, but the
    # app never loads it.  Remove it to prove the preload covers even the
    # worst case: a QML deploy WITHOUT Qt<major>Widgets near the exe.
    (app_dir / qdll("Widgets")).unlink(missing_ok=True)
    check(missing_dlls(app_dir, [qdll("Widgets")]) == [qdll("Widgets")],
          f"A: premise -- {qdll('Widgets')} removed from clean QML deploy")

    proc = start_app(app_dir / "qt-qml-test.exe")
    try:
        session = Session("scA", proc.pid, LIBRARY, work)
        port_file = work / "portA.txt"
        text = await attach_and_probe(session, proc.pid, port_file,
                                      "btnOK", "statusText")
        if session._writer:
            session._writer.close()
        check(text == "OK clicked!",
              f"A: click on QML btnOK -> status '{text}'")
        eject(proc.pid)
    finally:
        stop_app(proc)


async def scenario_widget(work: Path) -> None:
    # The widget app never links Quick/Qml (the library's own closure
    # dependencies) -- those must NOT appear in a clean widget deploy.
    # Qt6's windeployqt additionally drops Qt6Network next to the exe
    # (a Qt6Gui plugin dependency), so Network is not asserted absent.
    qml_dlls = [qdll("Quick"), qdll("Qml")]
    print(f"\n[Scenario B] Widget app, windeployqt-only dir "
          f"(no {', '.join(qml_dlls)})")
    app_dir = work / "app-widget"
    deploy_windeployqt(WIDGET_APP, None, app_dir)
    missing = missing_dlls(app_dir, qml_dlls)
    check(set(missing) == set(qml_dlls),
          f"B: premise -- Quick/Qml absent from clean widget deploy "
          f"(missing: {missing})")

    proc = start_app(app_dir / "qt-widget-test.exe")
    try:
        session = Session("scB", proc.pid, LIBRARY, work)
        port_file = work / "portB.txt"
        text = await attach_and_probe(session, proc.pid, port_file,
                                      "btnOk", "statusLabel")
        if session._writer:
            session._writer.close()
        check(text.startswith("Status: OK clicked!"),
              f"B: click on widget btnOk -> status '{text}'")
        eject(proc.pid)
    finally:
        stop_app(proc)


def scenario_diagnostic() -> None:
    print(f"\n[Scenario C] Missing closure DLL -> named diagnostic")
    moved = QTC_BIN / qdll("Qml")
    backup = QTC_BIN / f"{qdll('Qml')}.bak"
    if not moved.exists():
        print(f"  [SKIP] {qdll('Qml')} not in qt-commander bin; nothing to move")
        return
    shutil.move(moved, backup)
    try:
        proc = subprocess.run(
            [str(INJECTOR), "--list-deps", str(LIBRARY),
             "--search-dir", str(QTC_BIN)],
            capture_output=True, text=True, timeout=30)
        deps = json.loads(proc.stdout)["deps"]
        names = [Path(p).name.lower() for p in deps]
        check(f"qt{QT_MAJOR}qml{QT_DLL_SUFFIX}.dll" not in names,
              f"C: {qdll('Qml')} no longer in closure after removal")
        check(proc.returncode == 0, "C: --list-deps still exits 0")
    finally:
        shutil.move(backup, moved)


async def main() -> int:
    global QT_MAJOR, QT_KIT, QT_DLL_SUFFIX, QT_BIN, WINDEPLOYQT
    if not (INJECTOR.exists() and LIBRARY.exists()):
        print("ERROR: qt-injector.exe / libqt-commander.dll not found in "
              ".qt-commander/bin -- run qt_build first")
        return 2
    try:
        QT_MAJOR, QT_KIT, QT_DLL_SUFFIX = detect_deployment()
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 2
    print(f"Detected Qt{QT_MAJOR} ({QT_KIT}) library deployment "
          f"({QTC_BIN})")
    if QT_BIN is None:
        print("ERROR: Qt bin dir not found -- set QT_BIN env var or run "
              "qt_build first")
        return 2
    # windeployqt must match the DEPLOYED Qt major AND kit, not the invoking
    # build tree's (ctest passes its own QT_BIN; the deployment may be the
    # other major/kit after the last qt_build).  A wrong-major windeployqt
    # rejects the exe outright; a wrong-kit one deploys DLLs that crash the
    # app at startup (entry point not found).  Fall back to the canonical
    # install of the deployment's (major, kit).
    bin_major = qt_bin_major(QT_BIN)
    bin_kit = qt_bin_kit(QT_BIN)
    if (bin_major is not None and bin_major != QT_MAJOR) or (
            bin_kit is not None and bin_kit != QT_KIT):
        alt = _CANONICAL_QT_BIN.get((QT_MAJOR, QT_KIT))
        if alt and alt.is_dir():
            print(f"NOTE: QT_BIN ({QT_BIN}) is Qt{bin_major or '?'} "
                  f"({bin_kit or '?'}); deployment is Qt{QT_MAJOR} "
                  f"({QT_KIT}) -- using {alt} for windeployqt")
            QT_BIN = alt
            WINDEPLOYQT = QT_BIN / "windeployqt.exe"
        else:
            print(f"ERROR: QT_BIN ({QT_BIN}) is Qt{bin_major or '?'} "
                  f"({bin_kit or '?'}) but deployment is Qt{QT_MAJOR} "
                  f"({QT_KIT}); no matching install at {alt}")
            return 2
    if not WINDEPLOYQT.exists():
        print(f"ERROR: windeployqt not found at {WINDEPLOYQT}")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="qtc_preload_"))
    try:
        work = tmp
        await scenario_qml(work)
        await scenario_widget(work)
        scenario_diagnostic()
    finally:
        # Best-effort cleanup; injected apps may briefly hold DLL locks.
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{'='*60}")
    if failures:
        print(f"FAILED ({len(failures)}):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("ALL SCENARIOS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
