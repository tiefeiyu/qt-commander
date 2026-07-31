#!/usr/bin/env python3
"""Run all qt-commander tests.

Usage:
    python scripts/run_all_tests.py                           # Python only
    python scripts/run_all_tests.py --quick                   # Python + ctest
    python scripts/run_all_tests.py --skip-ctest              # Python only

    # With MSVC environment (enables ctest):
    python scripts/run_all_tests.py --vcvars "C:/.../vcvars64.bat" --qt-env "C:/Qt/.../qtenv2.bat"
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MSVC_BUILD = ROOT / "build" / "msvc"

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

    bat = (f'@echo off\r\n'
           f'call "{vcvars}" {vcvars_args}\r\n'
           f'call "{qt_env}"\r\n'
           f'cd /d {ROOT}\r\n'
           f'{cmd}\r\n')
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


def main():
    global passed, failed, skipped
    p = argparse.ArgumentParser(description="Run all qt-commander tests")
    p.add_argument("--skip-ctest", action="store_true",
                   help="Skip ctest (run Python only)")
    p.add_argument("--quick", action="store_true",
                   help="Skip CMake configure step (assumes build/msvc exists)")
    p.add_argument("--vcvars", default="",
                   help="Path to vcvars64.bat (required for ctest)")
    p.add_argument("--vcvars-args", default="amd64")
    p.add_argument("--qt-env", default="",
                   help="Path to qtenv2.bat (required for ctest)")
    args = p.parse_args()

    v, q = args.vcvars, args.qt_env

    # ── Python (always) ──
    print("=== Python (pytest) ===", flush=True)
    run("pytest", ["python", "-m", "pytest", "tests/unit_server/", "-q"])

    # ── C++ via MSVC ctest ──
    if args.skip_ctest:
        print("\n=== C++ tests: SKIPPED ===")
    elif not v or not q:
        print("\n=== C++ tests: SKIPPED (--vcvars and --qt-env required) ===")
        skipped += 1
    else:
        # Configure CMake if needed
        if not args.quick or not (MSVC_BUILD / "CMakeCache.txt").exists():
            print("\n=== CMake configure ===", flush=True)
            run_with_msvc("cmake configure",
                f'cmake -B build/msvc -G Ninja '
                f'-DBUILD_INJECTOR=ON -DBUILD_LIBRARY=ON -DBUILD_TESTS=ON '
                f'-DCMAKE_BUILD_TYPE=Release', v, q)

            if not (MSVC_BUILD / "CMakeCache.txt").exists():
                print("  ERROR: CMake configure failed, skipping ctest")
                skipped += 1
                print(f"\n{'='*40}")
                print(f"  Passed: {passed}  Failed: {failed}  Skipped: {skipped}")
                print("=" * 40)
                return 0 if failed == 0 else 1

        # Build
        print("\n=== CMake build ===", flush=True)
        run_with_msvc("cmake build",
            "cmake --build build/msvc", v, q)

        # Run ctest
        print("\n=== C++ (ctest) ===", flush=True)
        run_with_msvc("ctest",
            "cd build\\msvc && ctest --output-on-failure", v, q,
            timeout=300)

    print(f"\n{'='*40}")
    print(f"  Passed: {passed}  Failed: {failed}  Skipped: {skipped}")
    print("=" * 40)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
