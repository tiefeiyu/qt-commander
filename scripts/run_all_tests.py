#!/usr/bin/env python3
"""Run all qt-commander tests.

Usage:
    python scripts/run_all_tests.py                           # all (needs g++)
    python scripts/run_all_tests.py --quick                   # Python + MSVC ctest
    python scripts/run_all_tests.py --skip-cpp                # Python only
    python scripts/run_all_tests.py --skip-e2e                # skip Qt-dependent E2E

    # With MSVC environment (for ctest + E2E):
    python scripts/run_all_tests.py --vcvars "C:/.../vcvars64.bat" --qt-env "C:/Qt/.../qtenv2.bat"
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
MSVC = BUILD / "msvc"

passed = 0
failed = 0
skipped = 0


def run(name: str, cmd: list[str], cwd: Path = ROOT, timeout: int = 600,
        env: dict | None = None, skip_reason: str = "") -> bool:
    global passed, failed, skipped
    if skip_reason:
        print(f"  SKIP ({skip_reason}): {name}")
        skipped += 1
        return False
    print(f"  {name} ...", end=" ", flush=True)
    try:
        e = os.environ.copy()
        if env:
            e.update(env)
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout, env=e)
    except subprocess.TimeoutExpired:
        print("TIMEOUT")
        failed += 1
        return False
    if r.returncode == 0:
        print("PASS")
        passed += 1
        return True
    else:
        print("FAIL")
        if r.stderr.strip():
            print(f"    {r.stderr.strip()[-200:]}")
        if r.stdout.strip():
            print(f"    {r.stdout.strip()[-200:]}")
        failed += 1
        return False


def run_with_msvc(name: str, cmd: str, vcvars: str, qt_env: str,
                  vcvars_args: str = "amd64", timeout: int = 600) -> bool:
    """Run a command inside a vcvars + qtenv2 environment."""
    global passed, failed, skipped
    if not vcvars or not qt_env:
        run(name, [], skip_reason="--vcvars or --qt-env not set")
        return False

    bat = f'@echo off\r\ncall "{vcvars}" {vcvars_args}\r\ncall "{qt_env}"\r\ncd /d {ROOT}\r\n{cmd}\r\n'
    tmp = ROOT / "build" / "_run_msvc.bat"
    tmp.write_text(bat)
    print(f"  {name} ...", end=" ", flush=True)
    try:
        r = subprocess.run(["cmd", "/c", str(tmp)], capture_output=True,
                           text=True, timeout=timeout, cwd=ROOT)
    except subprocess.TimeoutExpired:
        print("TIMEOUT")
        failed += 1; return False
    tmp.unlink(missing_ok=True)
    if r.returncode == 0:
        print("PASS"); passed += 1; return True
    else:
        print("FAIL")
        for line in r.stdout.splitlines()[-5:]:
            print(f"    {line}")
        for line in r.stderr.splitlines()[-3:]:
            print(f"    {line}")
        failed += 1; return False


def gpp(src: str, out: str) -> bool:
    """Compile C++ source with g++."""
    libs = "-lws2_32 -lpsapi -lbcrypt"
    inc = "-I src/injector -I src/common -I src/library"
    r = subprocess.run(
        f"g++ -std=c++17 -static -o {out} {src} {inc} {libs}",
        shell=True, capture_output=True, text=True, cwd=ROOT)
    return r.returncode == 0


def main():
    global passed, failed, skipped
    p = argparse.ArgumentParser(description="Run all qt-commander tests")
    p.add_argument("--skip-cpp", action="store_true")
    p.add_argument("--skip-e2e", action="store_true")
    p.add_argument("--quick", action="store_true")
    p.add_argument("--vcvars", default="", help="Path to vcvars64.bat")
    p.add_argument("--vcvars-args", default="amd64")
    p.add_argument("--qt-env", default="", help="Path to qtenv2.bat")
    args = p.parse_args()

    v = args.vcvars
    q = args.qt_env

    # ── Python ──
    print("=== Python (pytest) ===", flush=True)
    run("pytest", ["python", "-m", "pytest", "tests/unit_server/", "-q"])

    # ── MSVC ctest ──
    print("\n=== MSVC ctest ===", flush=True)
    if MSVC.exists():
        run_with_msvc("ctest", "ctest --output-on-failure", v, q, args.vcvars_args)
    else:
        run("ctest", [], skip_reason="build/msvc not found")

    if args.quick:
        print(f"\n{'='*40}\n  Passed: {passed}  Failed: {failed}  Skipped: {skipped}\n{'='*40}")
        return 0 if failed == 0 else 1

    # ── g++ ──
    if args.skip_cpp:
        print("\n=== g++ tests: SKIPPED ===")
    elif subprocess.run(["g++", "--version"], capture_output=True).returncode != 0:
        run("g++", [], skip_reason="g++ not found")
    else:
        BUILD.mkdir(exist_ok=True)
        print("\n=== Building g++ binaries ===", flush=True)
        binaries = {
            "test_di.exe":     "tests/unit_injector/test_injector_logic.cpp  src/injector/injector_di.cpp",
            "test_pe.exe":     "tests/unit_injector/test_pe_real.cpp",
            "test_int.exe":    "tests/unit_injector/test_cli_integration.cpp src/injector/injector_di.cpp",
            "test_w32.exe":    "tests/unit_injector/test_win32_coverage.cpp",
            "test_sock.exe":   "tests/unit_injector/test_socket_utils.cpp",
            "test_injerr.exe": "tests/unit_injector/test_injector_errors.cpp src/injector/injector_win.cpp src/common/socket_utils.cpp",
            "test_e2e.exe":    "tests/unit_injector/test_e2e.cpp",
            "test_exit.exe":   "tests/unit_injector/test_e2e_exit_codes.cpp",
            "test_exit45.exe": "tests/unit_injector/test_exit45.cpp",
        }
        built = {}
        for out, src in binaries.items():
            full = f"build/{out}"
            ok = gpp(src, full)
            print(f"  {'OK' if ok else 'FAIL'}: {out}")
            if ok:
                built[out] = full

        # Run unit tests
        print("\n=== g++ unit tests ===", flush=True)
        for name, exe in [
            ("DI injector logic", "test_di.exe"),
            ("PE parser (real)", "test_pe.exe"),
            ("Win32ProcessOps + CLI", "test_int.exe"),
            ("injector_win helpers", "test_w32.exe"),
            ("Socket utils", "test_sock.exe"),
        ]:
            path = built.get(exe, "")
            run(name, [path], skip_reason="not built" if not path else "")

        # E2E (needs MSVC Qt env + built test app)
        if not args.skip_e2e:
            print("\n=== E2E tests ===", flush=True)
            for name, exe in [
                ("E2E full lifecycle", "test_e2e.exe"),
                ("Exit codes 3/6", "test_exit.exe"),
                ("Exit codes 4/5", "test_exit45.exe"),
                ("Injector error path", "test_injerr.exe"),
            ]:
                path = built.get(exe, "")
                if path:
                    run_with_msvc(name, str(ROOT / path), v, q, args.vcvars_args,
                                  timeout=120)
                else:
                    run(name, [], skip_reason="not built")

    print(f"\n{'='*40}")
    print(f"  Passed: {passed}  Failed: {failed}  Skipped: {skipped}")
    print("=" * 40)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
