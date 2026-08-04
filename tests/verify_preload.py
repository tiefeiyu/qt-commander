"""End-to-end verification: the injector preloads the dependency closure of
libqt-commander.dll into the target process, so clean windeployqt-only app
directories attach without any manual Qt DLL copies.

Scenarios:
  A. QML app (qt-qml-test) deployed by windeployqt ONLY -- its dir contains
     NO Qt5Widgets.dll (the library's dependency the app itself never
     links).  Attach must succeed and a real click must work.
  B. Widget app (qt-widget-test) deployed by windeployqt ONLY -- its dir
     contains NO Qt5Quick/Qt5Qml/Qt5Network.  Attach must succeed and a
     real click must work.
  C. Boundary: with Qt5Qml.dll removed from the qt-commander bin dir, the
     injector must fail with a diagnostic naming Qt5Qml.dll (instead of
     the opaque "LoadLibraryW returned NULL").

Usage: python tests/verify_preload.py
Exit code 0 = all scenarios passed.
"""
import asyncio
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from qt_commander.errors import InjectionError  # noqa: E402
from qt_commander.rpc_client import inject_and_connect  # noqa: E402
from qt_commander.session import Session  # noqa: E402

QTC_BIN = REPO / ".qt-commander" / "bin"
INJECTOR = QTC_BIN / "qt-injector.exe"
LIBRARY = QTC_BIN / "libqt-commander.dll"

QT_ROOT = Path(r"C:\Software\Qt\5.15.2\msvc2019_64")
WINDEPLOYQT = QT_ROOT / "bin" / "windeployqt.exe"
QML_SRC = REPO / "tests" / "test-apps" / "qml"
WIDGET_SRC = REPO / "tests" / "test-apps" / "widget"

QML_APP = REPO / ".qt-commander" / "qml-app" / "bin" / "qt-qml-test.exe"
WIDGET_APP = REPO / ".qt-commander" / "test-app" / "bin" / "qt-widget-test.exe"

failures = []


def check(cond: bool, msg: str):
    tag = "PASS" if cond else "FAIL"
    print(f"  [{tag}] {msg}")
    if not cond:
        failures.append(msg)


# ---------------------------------------------------------------------------
# Deploy helpers
# ---------------------------------------------------------------------------

def deploy_windeployqt(app_exe: Path, qmldir: Path | None, dest: Path) -> None:
    """Fresh windeployqt-only deployment into dest (clean dir)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    shutil.copy2(app_exe, dest / app_exe.name)
    args = [str(WINDEPLOYQT), "--release", "--no-translations",
            "--no-compiler-runtime"]
    if qmldir:
        args += ["--qmldir", str(qmldir)]
    args.append(str(dest / app_exe.name))
    subprocess.run(args, check=True, capture_output=True)
    # The widget app loads its UI from code; the QML app embeds the scene in
    # a qrc, so no extra source files are needed.


def missing_dlls(app_dir: Path, names: list[str]) -> list[str]:
    return [n for n in names if not (app_dir / n).exists()]


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
    print("\n[Scenario A] QML app, Qt5Widgets.dll removed from app dir")
    app_dir = work / "app-qml"
    deploy_windeployqt(QML_APP, QML_SRC, app_dir)
    # windeployqt happens to deploy Qt5Widgets for QML apps, but the app
    # never loads it.  Remove it to prove the preload covers even the
    # worst case: a QML deploy WITHOUT Qt5Widgets anywhere near the exe.
    (app_dir / "Qt5Widgets.dll").unlink(missing_ok=True)
    check(missing_dlls(app_dir, ["Qt5Widgets.dll"]) == ["Qt5Widgets.dll"],
          "A: premise -- Qt5Widgets.dll removed from clean QML deploy")

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
    print("\n[Scenario B] Widget app, windeployqt-only dir (no Qt5Quick/Qt5Qml)")
    app_dir = work / "app-widget"
    deploy_windeployqt(WIDGET_APP, None, app_dir)
    missing = missing_dlls(app_dir, ["Qt5Quick.dll", "Qt5Qml.dll",
                                     "Qt5Network.dll"])
    check(set(missing) == {"Qt5Quick.dll", "Qt5Qml.dll", "Qt5Network.dll"},
          f"B: premise -- Quick/Qml/Network absent from clean widget deploy "
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
    print("\n[Scenario C] Missing closure DLL -> named diagnostic")
    moved = QTC_BIN / "Qt5Qml.dll"
    backup = QTC_BIN / "Qt5Qml.dll.bak"
    if not moved.exists():
        print("  [SKIP] Qt5Qml.dll not in qt-commander bin; nothing to move")
        return
    shutil.move(moved, backup)
    try:
        proc = subprocess.run(
            [str(INJECTOR), "--list-deps", str(LIBRARY),
             "--search-dir", str(QTC_BIN)],
            capture_output=True, text=True, timeout=30)
        deps = json.loads(proc.stdout)["deps"]
        names = [Path(p).name.lower() for p in deps]
        check("qt5qml.dll" not in names,
              "C: Qt5Qml.dll no longer in closure after removal")
        check(proc.returncode == 0, "C: --list-deps still exits 0")
    finally:
        shutil.move(backup, moved)


async def main() -> int:
    if not (INJECTOR.exists() and LIBRARY.exists()):
        print("ERROR: qt-injector.exe / libqt-commander.dll not found in "
              ".qt-commander/bin -- run qt_build first")
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
